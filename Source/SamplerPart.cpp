#include "SamplerPart.h"

//==============================================================================
//  InsertFx
//==============================================================================
void InsertFx::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    dry.setSize (2, maxBlock);
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 2 };
    chorus.prepare (spec);
    phaser.prepare (spec);
    delay.prepare (spec);
    delayTimeSmoothed.reset (sampleRate, 0.08);
    reset();
}

void InsertFx::reset()
{
    chorus.reset();
    phaser.reset();
    delay.reset();
    tremPhase = 0.0;
    odLowpassState[0] = odLowpassState[1] = 0.0f;
}

void InsertFx::process (juce::AudioBuffer<float>& buffer, int n, const PartSettings& s)
{
    if (s.fxType != lastType)
    {
        reset();
        lastType = s.fxType;
    }

    if (s.fxType == Off || n <= 0)
        return;

    for (int ch = 0; ch < 2; ++ch)
        dry.copyFrom (ch, 0, buffer, ch, 0, n);

    auto block = juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    const float mix = s.fxMix;
    bool addDry = false; // true: izlaz = suvo + mokro*mix (eho)

    switch (s.fxType)
    {
        case Chorus:
            chorus.setRate (0.1f + s.fxRate * 4.9f);
            chorus.setDepth (juce::jlimit (0.0f, 1.0f, s.fxDepth));
            chorus.setCentreDelay (7.0f);
            chorus.setFeedback (0.0f);
            chorus.setMix (1.0f);
            chorus.process (ctx);
            break;

        case Phaser:
            phaser.setRate (0.05f + s.fxRate * 3.0f);
            phaser.setDepth (juce::jlimit (0.0f, 1.0f, s.fxDepth));
            phaser.setCentreFrequency (900.0f);
            phaser.setFeedback (0.5f);
            phaser.setMix (1.0f);
            phaser.process (ctx);
            break;

        case Tremolo:
        {
            const double inc = juce::MathConstants<double>::twoPi * (1.0 + s.fxRate * 11.0) / sr;
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float lfo = 0.5f + 0.5f * (float) std::sin (tremPhase);
                const float g = 1.0f - s.fxDepth * lfo;
                l[i] *= g;
                r[i] *= g;
                tremPhase += inc;
                if (tremPhase > juce::MathConstants<double>::twoPi)
                    tremPhase -= juce::MathConstants<double>::twoPi;
            }
            break;
        }

        case Overdrive:
        {
            const float drive = 1.0f + s.fxDepth * 24.0f;
            const float comp = 1.0f / (1.0f + s.fxDepth * 2.0f);
            const float cutoff = 1000.0f + s.fxRate * 11000.0f;
            const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * cutoff / (float) sr);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                float st = odLowpassState[ch];
                for (int i = 0; i < n; ++i)
                {
                    const float y = std::tanh (d[i] * drive) * comp;
                    st += a * (y - st);
                    d[i] = st;
                }
                odLowpassState[ch] = st;
            }
            break;
        }

        case Echo:
        {
            addDry = true;
            delayTimeSmoothed.setTargetValue ((float) ((0.06 + s.fxRate * 0.94) * sr));
            const float fb = s.fxDepth * 0.85f;
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float t = delayTimeSmoothed.getNextValue();
                const float dl = delay.popSample (0, t);
                const float dr = delay.popSample (1, t);
                delay.pushSample (0, l[i] + dr * fb);   // blagi ping-pong
                delay.pushSample (1, r[i] + dl * fb);
                l[i] = dl;
                r[i] = dr;
            }
            break;
        }

        default:
            break;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* w = buffer.getWritePointer (ch);
        auto* d = dry.getReadPointer (ch);
        if (addDry)
            for (int i = 0; i < n; ++i) w[i] = d[i] + w[i] * mix;
        else
            for (int i = 0; i < n; ++i) w[i] = d[i] * (1.0f - mix) + w[i] * mix;
    }
}

//==============================================================================
//  SamplerPart
//==============================================================================
SamplerPart::SamplerPart() {}

void SamplerPart::prepare (double newSampleRate, int maxBlock)
{
    sampleRate = newSampleRate;
    partBuffer.setSize (2, maxBlock);
    partBuffer.clear();
    insertFx.prepare (sampleRate, maxBlock);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 2 };
    for (auto& v : voices)
    {
        v.active = false;
        v.instrument.reset();
        v.env.setSampleRate (sampleRate);
        v.filter.prepare (spec);
        v.filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    }

    gainL.reset (sampleRate, 0.03);
    gainR.reset (sampleRate, 0.03);
    heldKeys.clear();
    heldKeys.reserve (128);
}

void SamplerPart::setInstrument (InstrumentPtr newInstrument)
{
    const juce::SpinLock::ScopedLockType sl (instrumentLock);
    std::swap (instrument, newInstrument);
    // stari instrument (newInstrument) se oslobadja ovde, na message niti
}

InstrumentPtr SamplerPart::getInstrument() const
{
    const juce::SpinLock::ScopedLockType sl (instrumentLock);
    return instrument;
}

int SamplerPart::getNumActiveVoices() const
{
    int n = 0;
    for (auto& v : voices)
        if (v.active) ++n;
    return n;
}

double SamplerPart::keyToPitch (int key) const
{
    return (double) (key + s.octave * 12 + s.transpose);
}

//==============================================================================
void SamplerPart::beginBlock (const PartSettings& settings, int numSamples)
{
    s = settings;

    {
        const juce::SpinLock::ScopedLockType sl (instrumentLock);
        blockInstrument = instrument;
    }

    envParams.attack  = juce::jmax (0.001f, s.attack);
    envParams.decay   = juce::jmax (0.001f, s.decay);
    envParams.sustain = s.sustain;
    envParams.release = juce::jmax (0.005f, s.release);

    glideCoef = s.portamento > 0.001f ? std::exp (-4.0 / (s.portamento * sampleRate)) : 0.0;

    filterOn = s.cutoff < 19500.0f || s.resonance > 0.01f;
    const float q = 0.7071f + s.resonance * s.resonance * 9.0f;
    for (auto& v : voices)
    {
        if (! v.active) continue;
        v.env.setParameters (envParams);
        if (filterOn)
        {
            v.filter.setCutoffFrequency (juce::jlimit (20.0f, (float) sampleRate * 0.45f, s.cutoff));
            v.filter.setResonance (q);
        }
    }

    const float g = juce::Decibels::decibelsToGain (s.volumeDb, -60.0f);
    gainL.setTargetValue (g * juce::jmin (1.0f, 1.0f - s.pan));
    gainR.setTargetValue (g * juce::jmin (1.0f, 1.0f + s.pan));

    partBuffer.clear (0, numSamples);
}

SamplerPart::Voice* SamplerPart::findFreeVoice()
{
    Voice* oldestReleased = nullptr;
    Voice* oldest = nullptr;
    for (auto& v : voices)
    {
        if (! v.active)
            return &v;
        if ((! v.keyDown || v.fadingOut) && (oldestReleased == nullptr || v.age < oldestReleased->age))
            oldestReleased = &v;
        if (oldest == nullptr || v.age < oldest->age)
            oldest = &v;
    }
    return oldestReleased != nullptr ? oldestReleased : oldest;
}

void SamplerPart::startVoice (Voice& v, int key, int velocity, double startPitch, double targetPitch)
{
    const int lookupNote = juce::jlimit (0, 127, (int) std::lround (targetPitch));
    auto* smp = blockInstrument->findSample (lookupNote, velocity);
    if (smp == nullptr)
        return;

    v.active = true;
    v.key = key;
    v.keyDown = true;
    v.held = false;
    v.instrument = blockInstrument;
    v.sample = smp;
    v.pos = (double) smp->startOffset;
    v.pitch = startPitch;
    v.pitchTarget = targetPitch;

    const float vel01 = (float) velocity / 127.0f;
    v.velGain = (1.0f - s.velSens) + s.velSens * std::pow (vel01, 1.6f);

    v.fadingOut = false;
    v.fadeGain = 1.0f;
    v.age = ++ageCounter;
    v.env.setParameters (envParams);
    v.env.reset();
    v.env.noteOn();
    v.filter.reset();
    if (filterOn)
    {
        v.filter.setCutoffFrequency (juce::jlimit (20.0f, (float) sampleRate * 0.45f, s.cutoff));
        v.filter.setResonance (0.7071f + s.resonance * s.resonance * 9.0f);
    }
}

void SamplerPart::releaseVoice (Voice& v)
{
    v.held = false;
    v.keyDown = false;
    v.env.noteOff();
}

void SamplerPart::noteOn (int key, int velocity)
{
    if (blockInstrument == nullptr || velocity <= 0)
        return;

    const double target = keyToPitch (key);

    if (s.mono)
    {
        heldKeys.erase (std::remove (heldKeys.begin(), heldKeys.end(), key), heldKeys.end());
        heldKeys.push_back (key);

        // trenutni mono glas (najnoviji koji jos svira)
        Voice* current = nullptr;
        for (auto& v : voices)
            if (v.active && ! v.fadingOut && (current == nullptr || v.age > current->age))
                current = &v;

        double startPitch = target;
        if (s.portamento > 0.001f)
        {
            if (current != nullptr)       startPitch = current->pitch;
            else if (lastPitch >= 0.0)    startPitch = lastPitch;
        }

        if (current != nullptr)
            current->fadingOut = true;    // brzo utisavanje prethodne note

        if (auto* v = findFreeVoice())
            startVoice (*v, key, velocity, startPitch, target);
    }
    else
    {
        // ista nota ponovo: pusti prethodnu da se ugasi
        for (auto& v : voices)
            if (v.active && v.key == key && ! v.fadingOut)
                releaseVoice (v);

        const double startPitch = (s.portamento > 0.001f && lastPitch >= 0.0) ? lastPitch : target;
        if (auto* v = findFreeVoice())
            startVoice (*v, key, velocity, startPitch, target);
    }

    lastPitch = target;
}

void SamplerPart::noteOff (int key)
{
    if (s.mono)
    {
        heldKeys.erase (std::remove (heldKeys.begin(), heldKeys.end(), key), heldKeys.end());

        for (auto& v : voices)
        {
            if (! v.active || v.fadingOut || v.key != key || ! v.keyDown)
                continue;

            if (! heldKeys.empty())
            {
                // legato: vrati se na poslednji drzani taster
                const int newKey = heldKeys.back();
                v.key = newKey;
                v.pitchTarget = keyToPitch (newKey);
                if (s.portamento <= 0.001f)
                    v.pitch = v.pitchTarget;
                lastPitch = v.pitchTarget;
            }
            else
            {
                v.keyDown = false;
                if (sustainDown) v.held = true;
                else releaseVoice (v);
            }
        }
        return;
    }

    for (auto& v : voices)
    {
        if (v.active && v.key == key && v.keyDown && ! v.fadingOut)
        {
            v.keyDown = false;
            if (sustainDown) v.held = true;
            else releaseVoice (v);
        }
    }
}

void SamplerPart::setSustain (bool down)
{
    sustainDown = down;
    if (! down)
        for (auto& v : voices)
            if (v.active && v.held && ! v.keyDown)
                releaseVoice (v);
}

void SamplerPart::allNotesOff (bool immediate)
{
    heldKeys.clear();
    for (auto& v : voices)
    {
        if (! v.active) continue;
        if (immediate) v.fadingOut = true;
        else releaseVoice (v);
    }
}

//==============================================================================
static inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b = w + a;
    return ((((a * t) - b) * t + c) * t + x0);
}

void SamplerPart::renderVoices (int start, int num)
{
    if (num <= 0)
        return;

    auto* outL = partBuffer.getWritePointer (0) + start;
    auto* outR = partBuffer.getWritePointer (1) + start;
    const double bendOffset = bend * (double) s.bendRange + s.fineCents / 100.0;
    const float fadeStep = 1.0f / (0.006f * (float) sampleRate);   // 6 ms

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const auto* smp = v.sample;
        const float* L = smp->buffer.getReadPointer (0);
        const float* R = smp->buffer.getReadPointer (1);
        const int len = smp->buffer.getNumSamples();
        const bool loop = smp->hasLoop;
        const int loopStart = smp->loopStart, loopEnd = smp->loopEnd;
        const int loopLen = loopEnd - loopStart;
        const double rateRatio = smp->sampleRate / sampleRate;

        auto fetch = [&] (const float* d, int idx) -> float
        {
            if (loop && idx >= loopEnd) idx -= loopLen;
            if (idx < 0) idx = 0;
            return idx < len ? d[idx] : 0.0f;
        };

        bool gliding = std::abs (v.pitchTarget - v.pitch) > 1.0e-4;
        double inc = std::exp2 ((v.pitch + bendOffset - smp->rootNote) / 12.0) * rateRatio;

        for (int i = 0; i < num; ++i)
        {
            if (gliding)
            {
                v.pitch = v.pitchTarget + (v.pitch - v.pitchTarget) * glideCoef;
                if (std::abs (v.pitchTarget - v.pitch) <= 1.0e-4)
                {
                    v.pitch = v.pitchTarget;
                    gliding = false;
                }
                inc = std::exp2 ((v.pitch + bendOffset - smp->rootNote) / 12.0) * rateRatio;
            }

            const int ip = (int) v.pos;
            const float t = (float) (v.pos - ip);
            float l = hermite (fetch (L, ip - 1), fetch (L, ip), fetch (L, ip + 1), fetch (L, ip + 2), t);
            float r = hermite (fetch (R, ip - 1), fetch (R, ip), fetch (R, ip + 1), fetch (R, ip + 2), t);

            float g = v.env.getNextSample() * v.velGain;
            if (v.fadingOut)
            {
                v.fadeGain -= fadeStep;
                if (v.fadeGain <= 0.0f)
                {
                    v.active = false;
                    break;
                }
                g *= v.fadeGain;
            }

            l *= g;
            r *= g;
            if (filterOn)
            {
                l = v.filter.processSample (0, l);
                r = v.filter.processSample (1, r);
            }
            outL[i] += l;
            outR[i] += r;

            v.pos += inc;
            if (loop)
            {
                while (v.pos >= (double) loopEnd)
                    v.pos -= (double) loopLen;
            }
            else if (v.pos >= (double) (len - 1))
            {
                v.active = false;
                break;
            }

            if (! v.env.isActive())
            {
                v.active = false;
                break;
            }
        }

        if (! v.active)
            v.instrument.reset();
    }
}

void SamplerPart::endBlock (juce::AudioBuffer<float>& mainOut,
                            juce::AudioBuffer<float>& reverbSend,
                            juce::AudioBuffer<float>& delaySend, int n)
{
    insertFx.process (partBuffer, n, s);

    auto* pl = partBuffer.getWritePointer (0);
    auto* pr = partBuffer.getWritePointer (1);
    auto* ml = mainOut.getWritePointer (0);
    auto* mr = mainOut.getWritePointer (1);
    auto* rl = reverbSend.getWritePointer (0);
    auto* rr = reverbSend.getWritePointer (1);
    auto* dl = delaySend.getWritePointer (0);
    auto* dr = delaySend.getWritePointer (1);

    for (int i = 0; i < n; ++i)
    {
        const float l = pl[i] * gainL.getNextValue();
        const float r = pr[i] * gainR.getNextValue();
        ml[i] += l;  mr[i] += r;
        rl[i] += l * s.reverbSend;  rr[i] += r * s.reverbSend;
        dl[i] += l * s.delaySend;   dr[i] += r * s.delaySend;
    }
}

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

void InsertFx::process (juce::AudioBuffer<float>& buffer, int n, int type, float rate, float depth, float mix)
{
    if (type != lastType)
    {
        reset();
        lastType = type;
    }

    if (type == Off || n <= 0)
        return;

    for (int ch = 0; ch < 2; ++ch)
        dry.copyFrom (ch, 0, buffer, ch, 0, n);

    auto block = juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    bool addDry = false;

    switch (type)
    {
        case Chorus:
            chorus.setRate (0.1f + rate * 4.9f);
            chorus.setDepth (juce::jlimit (0.0f, 1.0f, depth));
            chorus.setCentreDelay (7.0f);
            chorus.setFeedback (0.0f);
            chorus.setMix (1.0f);
            chorus.process (ctx);
            break;

        case Phaser:
            phaser.setRate (0.05f + rate * 3.0f);
            phaser.setDepth (juce::jlimit (0.0f, 1.0f, depth));
            phaser.setCentreFrequency (900.0f);
            phaser.setFeedback (0.5f);
            phaser.setMix (1.0f);
            phaser.process (ctx);
            break;

        case Tremolo:
        {
            const double inc = juce::MathConstants<double>::twoPi * (1.0 + rate * 11.0) / sr;
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float lfo = 0.5f + 0.5f * (float) std::sin (tremPhase);
                const float g = 1.0f - depth * lfo;
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
            const float drive = 1.0f + depth * 24.0f;
            const float comp = 1.0f / (1.0f + depth * 2.0f);
            const float cutoff = 1000.0f + rate * 11000.0f;
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
            delayTimeSmoothed.setTargetValue ((float) ((0.06 + rate * 0.94) * sr));
            const float fb = depth * 0.85f;
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float t = delayTimeSmoothed.getNextValue();
                const float dl = delay.popSample (0, t);
                const float dr = delay.popSample (1, t);
                delay.pushSample (0, l[i] + dr * fb);
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
    heldKeys.reserve (numIds);
    idPitch.fill (60);
}

int SamplerPart::getNumActiveVoices() const
{
    int n = 0;
    for (auto& v : voices)
        if (v.active) ++n;
    return n;
}

void SamplerPart::beginBlock (const PartRuntime* runtime, int numSamples)
{
    cfg = runtime;
    partBuffer.clear (0, numSamples);

    if (cfg == nullptr)
        return;

    const auto& u = cfg->user;
    envParams.attack  = juce::jmax (0.001f, u.attack);
    envParams.decay   = juce::jmax (0.001f, u.decay);
    envParams.sustain = u.sustain;
    envParams.release = juce::jmax (0.005f, u.release);

    glideCoef = u.portamento > 0.001f ? std::exp (-4.0 / (u.portamento * sampleRate)) : 0.0;

    filterOn = u.cutoff < 19500.0f || u.resonance > 0.01f;
    filterQ = 0.7071f + u.resonance * u.resonance * 9.0f;
    const float fc = juce::jlimit (20.0f, (float) sampleRate * 0.45f, u.cutoff);
    for (auto& v : voices)
    {
        if (! v.active) continue;
        v.env.setParameters (envParams);
        if (filterOn)
        {
            v.filter.setCutoffFrequency (fc);
            v.filter.setResonance (filterQ);
        }
    }

    const auto& p = cfg->part;
    const float g = cfg->active ? juce::Decibels::decibelsToGain (p.volumeDb, -60.0f) : 0.0f;
    gainL.setTargetValue (g * juce::jmin (1.0f, 1.0f - p.pan));
    gainR.setTargetValue (g * juce::jmin (1.0f, 1.0f + p.pan));
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

void SamplerPart::startVoices (int id, int pitch, int velocity, double startPitch)
{
    const auto& u = cfg->user;
    const juce::uint32 serial = ++serialCounter;
    if (id < 128)
        monoSerial = serial;

    const float vel01 = (float) velocity / 127.0f;
    const float velGain = (1.0f - u.velSens) + u.velSens * std::pow (vel01, 1.6f);

    for (int i = 0; i < kNumOsc; ++i)
    {
        const auto& o = u.osc[(size_t) i];
        const auto& inst = cfg->inst[(size_t) i];
        if (! o.on || inst == nullptr)
            continue;
        if (pitch < o.keyLow || pitch > o.keyHigh || velocity < o.velLow || velocity > o.velHigh)
            continue;

        const int oscPitch = pitch + o.transpose;
        auto* zone = inst->findZone (juce::jlimit (0, 127, oscPitch), velocity);
        if (zone == nullptr || zone->sample < 0 || zone->sample >= (int) inst->samples.size())
            continue;
        auto* smp = inst->samples[(size_t) zone->sample].get();

        auto* vp = findFreeVoice();
        if (vp == nullptr)
            return;
        auto& v = *vp;

        v.active = true;
        v.id = id;
        v.serial = serial;
        v.keyDown = true;
        v.held = false;
        v.instrument = inst;
        v.sample = smp;
        v.oscTranspose = o.transpose;
        v.pos = (double) smp->startOffset;
        v.pitch = startPitch + o.transpose;
        v.pitchTarget = (double) oscPitch;
        v.rootKey = zone->rootKey;
        v.tune = (zone->tuneCents + o.fineCents) / 100.0;
        v.fixedPitch = zone->fixedPitch;

        const int len = smp->buffer.getNumSamples();
        if (zone->hasOwnLoop() && zone->loopEnd <= len)
        {
            v.loop = zone->loop;
            v.loopStart = zone->loopStart;
            v.loopEnd = zone->loopEnd;
        }
        else
        {
            v.loop = smp->hasLoop && zone->loop;
            v.loopStart = smp->loopStart;
            v.loopEnd = smp->loopEnd;
        }
        if (zone->sampleStart > 0 && zone->sampleStart < len - 64)
            v.pos = v.loop ? juce::jmin ((double) zone->sampleStart, (double) v.loopStart) : (double) zone->sampleStart;

        const float pan = juce::jlimit (-1.0f, 1.0f, zone->pan + o.pan);
        const float g = juce::Decibels::decibelsToGain (zone->gainDb + o.levelDb);
        v.velGain = velGain;
        v.gainL = g * juce::jmin (1.0f, 1.0f - pan);
        v.gainR = g * juce::jmin (1.0f, 1.0f + pan);

        v.fadingOut = false;
        v.fadeGain = 1.0f;
        v.age = ++ageCounter;
        v.env.setParameters (envParams);
        v.env.reset();
        v.env.noteOn();
        v.filter.reset();
        if (filterOn)
        {
            v.filter.setCutoffFrequency (juce::jlimit (20.0f, (float) sampleRate * 0.45f, u.cutoff));
            v.filter.setResonance (filterQ);
        }
    }
}

void SamplerPart::releaseVoice (Voice& v)
{
    v.held = false;
    v.keyDown = false;
    v.env.noteOff();
}

void SamplerPart::noteOn (int id, int pitch, int velocity, bool additive)
{
    if (cfg == nullptr || ! cfg->active || velocity <= 0 || id < 0 || id >= numIds)
        return;

    const auto& u = cfg->user;
    pitch = juce::jlimit (0, 127, pitch);
    idPitch[(size_t) id] = pitch;
    const bool porta = u.portamento > 0.001f;

    if (u.mono && id < 128)
    {
        heldKeys.erase (std::remove (heldKeys.begin(), heldKeys.end(), id), heldKeys.end());
        heldKeys.push_back (id);

        // glas koji trenutno svira (mono)
        double startPitch = pitch;
        bool found = false;
        for (auto& v : voices)
        {
            if (v.active && ! v.fadingOut && v.id < 128 && v.serial == monoSerial)
            {
                if (! found && porta)
                {
                    startPitch = v.pitch - v.oscTranspose;
                    found = true;
                }
                v.fadingOut = true;
            }
        }
        if (! found && porta && lastPitch >= 0.0)
            startPitch = lastPitch;

        startVoices (id, pitch, velocity, startPitch);
    }
    else
    {
        if (! additive)
            for (auto& v : voices)
                if (v.active && v.id == id && ! v.fadingOut)
                    releaseVoice (v);

        const double startPitch = (porta && lastPitch >= 0.0 && id < 128) ? lastPitch : (double) pitch;
        startVoices (id, pitch, velocity, startPitch);
    }

    if (id < 128)
        lastPitch = pitch;
}

void SamplerPart::noteOff (int id)
{
    if (id < 0 || id >= numIds)
        return;

    const bool sustainOn = sustainDown && (cfg == nullptr || cfg->part.sustainPedal);

    if (cfg != nullptr && cfg->user.mono && id < 128)
    {
        heldKeys.erase (std::remove (heldKeys.begin(), heldKeys.end(), id), heldKeys.end());

        bool retargeted = false;
        for (auto& v : voices)
        {
            if (! v.active || v.fadingOut || v.id != id || ! v.keyDown)
                continue;

            if (! heldKeys.empty())
            {
                // legato: vrati se na poslednji drzani taster
                const int newId = heldKeys.back();
                const int newPitch = idPitch[(size_t) newId];
                v.id = newId;
                v.pitchTarget = newPitch + v.oscTranspose;
                if (cfg->user.portamento <= 0.001f)
                    v.pitch = v.pitchTarget;
                lastPitch = newPitch;
                retargeted = true;
            }
            else
            {
                v.keyDown = false;
                if (sustainOn) v.held = true;
                else releaseVoice (v);
            }
        }
        juce::ignoreUnused (retargeted);
        return;
    }

    for (auto& v : voices)
    {
        if (v.active && v.id == id && v.keyDown && ! v.fadingOut)
        {
            v.keyDown = false;
            if (sustainOn) v.held = true;
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
    const double bendOffset = cfg != nullptr ? bend * (double) cfg->user.bendRange : 0.0;
    const float fadeStep = 1.0f / (0.006f * (float) sampleRate);   // 6 ms

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const auto* smp = v.sample;
        const float* L = smp->buffer.getReadPointer (0);
        const float* R = smp->buffer.getReadPointer (1);
        const int len = smp->buffer.getNumSamples();
        const bool loop = v.loop;
        const int loopStart = v.loopStart, loopEnd = v.loopEnd;
        const int loopLen = loopEnd - loopStart;
        const double rateRatio = smp->sampleRate / sampleRate;
        const double pitchOffset = bendOffset + v.tune - v.rootKey;
        auto incFor = [&] (double p) { return std::exp2 (((v.fixedPitch ? v.rootKey : p) + pitchOffset) / 12.0) * rateRatio; };

        auto fetch = [&] (const float* d, int idx) -> float
        {
            if (loop && idx >= loopEnd) idx -= loopLen;
            if (idx < 0) idx = 0;
            return idx < len ? d[idx] : 0.0f;
        };

        bool gliding = std::abs (v.pitchTarget - v.pitch) > 1.0e-4;
        double inc = incFor (v.pitch);

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
                inc = incFor (v.pitch);
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

            l *= g * v.gainL;
            r *= g * v.gainR;
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

void SamplerPart::endBlock (juce::AudioBuffer<float>& mainOut, juce::AudioBuffer<float>& reverbSend,
                            juce::AudioBuffer<float>& delaySend, int n)
{
    if (cfg != nullptr)
    {
        const auto& u = cfg->user;
        insertFx.process (partBuffer, n, u.fxType, u.fxRate, u.fxDepth, u.fxMix);
    }

    const float rev = cfg != nullptr ? cfg->part.reverbSend : 0.0f;
    const float dly = cfg != nullptr ? cfg->part.delaySend : 0.0f;

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
        rl[i] += l * rev;  rr[i] += r * rev;
        dl[i] += l * dly;  dr[i] += r * dly;
    }
}

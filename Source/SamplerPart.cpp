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
//  KronosEnv
//==============================================================================
void KronosEnv::enter (Stage s, float tgt, float seconds)
{
    stage = s;
    target = tgt;
    remaining = juce::jmax (1, (int) (seconds * sr));
    step = (target - level) / (float) remaining;
}

void KronosEnv::start (const EnvSettings& e, double sampleRate)
{
    env = e;
    sr = sampleRate;
    level = e.start;
    enter (Attack, e.attackLevel, e.attackTime);
}

void KronosEnv::release()
{
    if (stage == Idle || stage == Release)
        return;
    if (std::abs (level - env.releaseLevel) < 1.0e-4f)
    {
        stage = Idle;   // vec je na krajnjem nivou (npr. nota pustena odmah)
        level = env.releaseLevel;
        return;
    }
    enter (Release, env.releaseLevel, env.releaseTime);
}

float KronosEnv::advance (int n)
{
    while (n > 0 && stage != Idle && stage != Sustain)
    {
        const int k = juce::jmin (n, remaining);
        level += step * (float) k;
        remaining -= k;
        n -= k;
        if (remaining <= 0)
        {
            level = target;
            switch (stage)
            {
                case Attack:  enter (Decay, env.breakLevel, env.decayTime); break;
                case Decay:   enter (Slope, env.sustain, env.slopeTime); break;
                case Slope:   stage = Sustain; level = env.sustain; break;
                case Release: stage = Idle; break;
                case Idle: case Sustain: break;
            }
        }
    }
    return level;
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
        v.filter.prepare (spec);
        v.filter2.prepare (spec);
    }

    gainL.reset (sampleRate, 0.03);
    gainR.reset (sampleRate, 0.03);
    for (auto& row : lanes)
        for (auto& L : row) { L.held.clear(); L.held.reserve (128); L.key = -1; L.artic = -1; }
    polyMaskOf.fill (0);
    idPitch.fill (60);
    idVel.fill (100);
    polyHeld = 0;
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

    const auto& p = cfg->part;
    float g = cfg->active ? juce::Decibels::decibelsToGain (p.volumeDb, -60.0f) : 0.0f;
    // vector joystick (Kronos Vector Volume): u centru jacina = Volume Center, ka svom smeru raste do 100%, na suprotnu stranu pada ka 0
    if (cfg->vectorOn && p.vectorDir > 0)
    {
        const float a = p.vectorDir == 1 ? vecX : p.vectorDir == 2 ? -vecX : p.vectorDir == 3 ? vecY : -vecY;
        const float c = juce::jlimit (0.0f, 1.0f, p.vectorCenter);
        float v = a >= 0.0f ? c + (1.0f - c) * a : c * (1.0f + a);
        v = juce::jlimit (0.0f, 1.0f, v);
        if (cfg->vectorEqualPower) v = std::sin (v * juce::MathConstants<float>::halfPi);
        g *= v;
    }
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

void SamplerPart::setGlide (Voice& v, double fromPitch, bool glide)
{
    const auto& u = cfg->user;
    if (! glide || fromPitch < 0.0)
    {
        v.pitch = v.pitchTarget;
        v.glideCoef16 = 0.0;
        return;
    }
    v.pitch = fromPitch;
    double t = juce::jmax (0.001, (double) u.portamento);
    if (u.portaConstRate)   // vreme po oktavi
        t = juce::jmax (0.001, t * std::abs (v.pitchTarget - fromPitch) / 12.0);
    const double coef = std::exp (-4.0 / (t * sampleRate));
    v.glideCoef16 = std::pow (coef, (double) controlRate);
}

void SamplerPart::startVoices (int id, int pitch, int velocity, double startPitch, bool glide,
                               juce::uint32 blockMask, juce::uint32 serial, int lane, int slot, bool keyOff)
{
    const auto& u = cfg->user;
    const float vel01 = (float) velocity / 127.0f;

    for (int i = 0; i < kNumOsc; ++i)
    {
        const auto& o = u.osc[(size_t) i];
        const auto& inst = cfg->inst[(size_t) i];
        if (! o.on || inst == nullptr || o.keyOff != keyOff || (blockMask & (1u << (i / kMsPerOsc))) == 0)
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
        const int oscBlock = i / kMsPerOsc;
        const auto& sb = u.block[(size_t) oscBlock];

        v.active = true;
        v.id = keyOff ? -1 : id;
        v.serial = serial;
        v.lane = lane;
        v.slot = slot;
        v.keyDown = ! keyOff;
        v.held = false;
        v.instrument = inst;
        v.sample = smp;
        v.block = oscBlock;
        v.oscTranspose = o.transpose;
        v.pos = (double) smp->startOffset;
        v.pitchTarget = (double) oscPitch;
        setGlide (v, startPitch >= 0.0 ? startPitch + o.transpose : -1.0, glide);
        v.rootKey = zone->rootKey;
        v.tune = (zone->tuneCents + o.fineCents) / 100.0;
        v.fixedPitch = zone->fixedPitch;

        const int len = smp->buffer.getNumSamples();
        v.endPos = zone->sampleEnd > 16 ? juce::jmin (zone->sampleEnd, len) : (smp->endPoint > 0 ? juce::jmin (smp->endPoint, len) : len);
        if (zone->hasOwnLoop() && zone->loopEnd <= len)
        {
            v.loop = zone->loop;
            v.loopStart = zone->loopStart;
            v.loopEnd = zone->loopEnd;
            v.loopXfade = juce::jlimit (0, juce::jmin (zone->loopStart, (zone->loopEnd - zone->loopStart) / 2), zone->loopXfade);
        }
        else
        {
            v.loop = smp->hasLoop && zone->loop;
            v.loopStart = smp->loopStart;
            v.loopEnd = smp->loopEnd;
            v.loopXfade = 0;
        }
        if (keyOff)
            v.loop = false;   // keyoff sempl se odsvira jednom
        if (zone->sampleStart > 0 && zone->sampleStart < v.endPos - 16)
            v.pos = v.loop ? juce::jmin ((double) zone->sampleStart, (double) v.loopStart) : (double) zone->sampleStart;

        const float pan = juce::jlimit (-1.0f, 1.0f, zone->pan + o.pan);
        float xfade = 1.0f;
        auto fade = [&o] (int steps, int dist)
        {
            if (steps <= 0) return 1.0f;
            const float x = juce::jlimit (0.0f, 1.0f, (float) (dist + 1) / (float) (steps + 1));
            return o.fadeCurve == 1 ? std::sin (x * juce::MathConstants<float>::halfPi) : x;
        };
        xfade *= fade (o.fadeIn, velocity - o.velLow);
        xfade *= fade (o.fadeOut, o.velHigh - velocity);
        // izjednacena dinamika: od donje granice (velRampDb) do vrha sloja (0 dB), glatko kroz ceo opseg
        float rampDb = 0.0f;
        if (o.velRampDb < 0.0f)
        {
            const int top = juce::jmax (o.velLow + 1, o.velHigh - o.fadeOut);
            const float t = juce::jlimit (0.0f, 1.0f, (float) (velocity - o.velLow) / (float) (top - o.velLow));
            rampDb = o.velRampDb * (1.0f - t);
        }
        const float g = xfade * juce::Decibels::decibelsToGain (zone->gainDb + o.levelDb + rampDb);
        v.vel01 = vel01;
        v.velGain = (1.0f - sb.velSens) + sb.velSens * std::pow (vel01, juce::jmax (0.1f, sb.velCurve));
        v.gainL = g * juce::jmin (1.0f, 1.0f - pan);
        v.gainR = g * juce::jmin (1.0f, 1.0f + pan);

        v.fadingOut = false;
        v.fadeGain = 1.0f;
        v.age = ++ageCounter;
        v.time = 0.0;
        v.vibPhase = 0.0;
        v.fltPhase = 0.0;
        v.ctrl = 0;

        auto amp = sb.ampEg;
        amp.releaseLevel = 0.0f;
        v.ampEnv.start (amp, sampleRate);
        v.pitchEnv.start (sb.pitchEg, sampleRate);
        v.filterEnv.start (sb.filterEg, sampleRate);

        v.filterOn = sb.filterType != FilterType::Off;
        v.cascade = sb.filterType == FilterType::LP24;
        using FT = juce::dsp::StateVariableTPTFilterType;
        const auto type = sb.filterType == FilterType::HP ? FT::highpass
                        : sb.filterType == FilterType::BP ? FT::bandpass : FT::lowpass;
        v.filter.setType (type);
        v.filter2.setType (FT::lowpass);
        v.filter.reset();
        v.filter2.reset();

        v.eqOn = o.hasEq();
        if (v.eqOn)
        {
            v.eq[0].set (Biquad::LowShelf, sampleRate, 150.0, 0.707, o.eqLowDb);
            v.eq[1].set (Biquad::Peak, sampleRate, juce::jlimit (100.0, 8000.0, (double) o.eqMidFreq), 0.9, o.eqMidDb);
            v.eq[2].set (Biquad::HighShelf, sampleRate, 6000.0, 0.707, o.eqHighDb);
            for (auto& q : v.eq) q.reset();
        }
        updateModulation (v);
    }
}

void SamplerPart::updateModulation (Voice& v)
{
    const auto& u = cfg->user;
    const auto& sb = u.block[(size_t) v.block];
    const double dt = (double) controlRate / sampleRate;

    // glide
    if (v.glideCoef16 > 0.0)
    {
        v.pitch = v.pitchTarget + (v.pitch - v.pitchTarget) * v.glideCoef16;
        if (std::abs (v.pitchTarget - v.pitch) < 1.0e-3)
        {
            v.pitch = v.pitchTarget;
            v.glideCoef16 = 0.0;
        }
    }
    else
    {
        v.pitch = v.pitchTarget;
    }

    // pitch: EG + vibrato + bend
    const float pEg = v.pitchEnv.advance (controlRate);
    const double delayFade = juce::jlimit (0.0, 1.0, (v.time - sb.vibDelay) / juce::jmax (0.005, (double) sb.vibFade));
    const double vibCents = sb.vibDepth * delayFade + modWheel * sb.mwToVib + aftertouch * sb.atToVib;
    v.vibPhase += juce::MathConstants<double>::twoPi * sb.vibRate * dt;
    if (v.vibPhase > juce::MathConstants<double>::twoPi) v.vibPhase -= juce::MathConstants<double>::twoPi;
    const double bendSemis = bend >= 0.0f ? bend * u.bendUp : bend * u.bendDown;
    const double pitchMod = pEg * sb.pitchEgInt + (std::sin (v.vibPhase) * (1.0 - std::abs ((double) sb.vibOffset)) + (double) sb.vibOffset) * vibCents / 100.0 + bendSemis + v.tune;

    const double rateRatio = v.sample->sampleRate / sampleRate;
    const double played = v.fixedPitch ? v.rootKey : v.pitch;
    v.inc = std::exp2 ((played + pitchMod - v.rootKey) / 12.0) * rateRatio;

    // filter
    const float fEg = v.filterEnv.advance (controlRate);
    v.fltPhase += juce::MathConstants<double>::twoPi * sb.filterLfoRate * dt;
    if (v.fltPhase > juce::MathConstants<double>::twoPi) v.fltPhase -= juce::MathConstants<double>::twoPi;

    if (v.filterOn)
    {
        const double octaves = sb.keyTrack * (v.pitch - v.oscTranspose - 60.0) / 12.0
                             + sb.velToCutoff * (v.vel01 - 1.0)
                             + (sb.filterEgInt + sb.velToFilterEg * v.vel01) * fEg
                             + sb.bendToCutoff * bend
                             + sb.filterLfoDepth * std::sin (v.fltPhase)
                             + sb.mwToCutoff * modWheel + sb.atToCutoff * aftertouch;
        const float fc = (float) juce::jlimit (20.0, sampleRate * 0.45, sb.cutoff * std::exp2 (octaves));
        const float q = 0.7071f + sb.resonance * sb.resonance * 9.0f;
        v.filter.setCutoffFrequency (fc);
        v.filter.setResonance (q);
        if (v.cascade)
        {
            v.filter2.setCutoffFrequency (fc);
            v.filter2.setResonance (0.7071f);
        }
    }

    v.ampMod = sb.atToAmp > 0.001f ? juce::Decibels::decibelsToGain (sb.atToAmp * aftertouch) : 1.0f;
    v.time += dt;
}

void SamplerPart::releaseVoice (Voice& v)
{
    v.held = false;
    v.keyDown = false;
    v.ampEnv.release();
    v.pitchEnv.release();
    v.filterEnv.release();
}

int SamplerPart::chooseMonoKey (const MonoLane& L) const
{
    if (L.held.empty())
        return -1;
    switch (cfg != nullptr ? cfg->user.priority : Priority::Last)
    {
        case Priority::High:
        {
            int best = L.held.front();
            for (int k : L.held) if (idPitch[(size_t) k] > idPitch[(size_t) best]) best = k;
            return best;
        }
        case Priority::Low:
        {
            int best = L.held.front();
            for (int k : L.held) if (idPitch[(size_t) k] < idPitch[(size_t) best]) best = k;
            return best;
        }
        default:
            return L.held.back();
    }
}

// osnovni OSC grupe prelaza: ukljucen prelaz, bez intervala (ako takvog nema, prvi iz grupe)
int SamplerPart::articDefault() const
{
    const auto& u = cfg->user;
    int first = -1;
    for (int b = 0; b < kNumBlocks; ++b)
    {
        if (! u.block[(size_t) b].articOn) continue;
        if (u.block[(size_t) b].articMask == 0) return b;
        if (first < 0) first = b;
    }
    return first;
}

bool SamplerPart::laneHasRange (int lane, int pitch) const
{
    const auto& u = cfg->user;
    int b = lane;
    if (lane == articLane)
    {
        b = articDefault();
        if (b < 0) return false;
    }
    const auto& sb = u.block[(size_t) b];
    return pitch >= sb.monoLow && pitch <= sb.monoHigh;
}

void SamplerPart::soundMono (int lane, int slot, int id, bool transition)
{
    const auto& u = cfg->user;
    auto& L = lanes[lane][slot];
    const int pitch = idPitch[(size_t) id];

    bool haveCurrent = false;
    double currentPitch = -1.0;
    for (auto& v : voices)
        if (v.active && ! v.fadingOut && v.lane == lane && v.slot == slot && v.serial == L.serial && (v.keyDown || v.held))
        {
            if (! haveCurrent) currentPitch = v.pitch - v.oscTranspose;
            haveCurrent = true;
        }

    // koji OSC svira
    juce::uint32 mask = 1u << lane;
    bool switching = false;
    int chosen = lane;
    if (lane == articLane)
    {
        chosen = articDefault();
        if (transition && L.key >= 0 && L.artic >= 0)
        {
            const int interval = pitch - idPitch[(size_t) L.key];
            const int dist = std::abs (interval);
            chosen = L.artic;   // bez pogodjenog intervala ostaje OSC koji vec svira (legato)
            for (int b = 0; b < kNumBlocks && dist >= 1 && dist <= 24; ++b)
            {
                const auto& sb = u.block[(size_t) b];
                if (! sb.articOn || (sb.articMask & (1 << (dist - 1))) == 0) continue;
                if ((sb.articDir == 1 && interval < 0) || (sb.articDir == 2 && interval > 0)) continue;
                chosen = b;
                break;
            }
            switching = chosen != L.artic;
        }
        if (chosen < 0) return;
        mask = 1u << chosen;
    }

    const bool glide = u.portaOn && (! u.portaFingered || transition);
    const bool legato = u.voiceMode == VoiceMode::MonoLegato && transition && haveCurrent && ! switching;

    if (legato)
    {
        // legato: ista nota nastavlja, menja se samo visina (bez novog attack-a)
        for (auto& v : voices)
        {
            if (! v.active || v.fadingOut || v.lane != lane || v.slot != slot || v.serial != L.serial)
                continue;
            const double from = v.pitch;
            v.id = id;
            v.keyDown = true;
            v.held = false;
            v.pitchTarget = pitch + v.oscTranspose;
            setGlide (v, from, glide);
        }
    }
    else
    {
        const double from = haveCurrent ? currentPitch : L.lastPitch;
        for (auto& v : voices)
            if (v.active && ! v.fadingOut && v.lane == lane && v.slot == slot && v.serial == L.serial)
            {
                if (switching) releaseVoice (v);   // prelaz: prethodni MS se gasi svojim release-om
                else v.fadingOut = true;
            }
        L.serial = ++serialCounter;
        startVoices (id, pitch, idVel[(size_t) id], glide ? from : -1.0, glide, mask, L.serial, lane, slot);
    }

    if (lane == articLane) L.artic = chosen;
    L.key = id;
    L.lastPitch = pitch;
}

void SamplerPart::laneNoteOn (int lane, int slot, int id)
{
    auto& L = lanes[lane][slot];
    const bool transition = ! L.held.empty();
    L.held.erase (std::remove (L.held.begin(), L.held.end(), id), L.held.end());
    L.held.push_back (id);
    const int target = chooseMonoKey (L);
    if (target == id && (target != L.key || ! transition))
        soundMono (lane, slot, id, transition);
}

void SamplerPart::laneNoteOff (int lane, int slot, int id, bool sustainOn)
{
    auto& L = lanes[lane][slot];
    const auto it = std::find (L.held.begin(), L.held.end(), id);
    if (it == L.held.end())
        return;
    L.held.erase (it);
    if (id != L.key)
        return;

    if (! L.held.empty())
    {
        soundMono (lane, slot, chooseMonoKey (L), true);   // vrati se na drzani taster
        return;
    }

    for (auto& v : voices)
    {
        if (! v.active || v.fadingOut || v.lane != lane || v.slot != slot || v.serial != L.serial || ! v.keyDown)
            continue;
        v.keyDown = false;
        if (sustainOn) v.held = true;
        else releaseVoice (v);
    }

    // keyoff sempl za OSC koji je svirao
    const int b = lane == articLane ? L.artic : lane;
    if (b >= 0 && slot == 0)
        startVoices (-1, idPitch[(size_t) id], idVel[(size_t) id], -1.0, false, 1u << b, ++serialCounter, -1, 0, true);

    L.key = -1;
    L.artic = -1;
}

void SamplerPart::noteOn (int id, int pitch, int velocity, bool additive)
{
    if (cfg == nullptr || ! cfg->active || velocity <= 0 || id < 0 || id >= 256)
        return;

    // drugi glas terce ima svoj id (256+), da bi mono terca imala dve nezavisne trake
    if (additive && id >= 128)
        id += 128;
    const int slot = id / 128;

    const auto& u = cfg->user;
    pitch = juce::jlimit (0, 127, pitch);
    idPitch[(size_t) id] = pitch;
    idVel[(size_t) id] = velocity;

    // koji OSC-ovi postoje
    juce::uint32 present = 0;
    for (int i = 0; i < kNumOsc; ++i)
        if (u.osc[(size_t) i].on && cfg->inst[(size_t) i] != nullptr)
            present |= 1u << (i / kMsPerOsc);

    juce::uint32 polyMask = 0;
    bool articMono = false;
    const int articDef = articDefault();
    for (int b = 0; b < kNumBlocks; ++b)
    {
        if ((present & (1u << b)) == 0) continue;
        const auto& sb = u.block[(size_t) b];
        if (sb.articOn)
        {
            if (u.isMono() && laneHasRange (articLane, pitch)) articMono = true;
            else if (b == articDef) polyMask |= 1u << b;   // van mono opsega svira samo osnovni OSC
            continue;
        }
        if (u.isMono() && pitch >= sb.monoLow && pitch <= sb.monoHigh)
            laneNoteOn (b, slot, id);
        else
            polyMask |= 1u << b;
    }
    if (articMono)
        laneNoteOn (articLane, slot, id);

    if (polyMask != 0)
    {
        for (auto& v : voices)
            if (v.active && v.id == id && v.lane < 0 && ! v.fadingOut)
                releaseVoice (v);

        // portamento samo kad je ceo user poly (mono user van opsega svira poly bez portamenta)
        const bool glide = ! u.isMono() && u.portaOn && id < 128 && lastPitch >= 0.0 && (! u.portaFingered || polyHeld > 0);
        startVoices (id, pitch, velocity, glide ? lastPitch : -1.0, glide, polyMask, ++serialCounter, -1, slot);
        polyMaskOf[(size_t) id] = polyMask;
        if (id < 128)
        {
            lastPitch = pitch;
            ++polyHeld;
        }
    }
    else
    {
        polyMaskOf[(size_t) id] = 0;
    }
}

void SamplerPart::noteOff (int id)
{
    if (id < 0 || id >= 256)
        return;
    noteOffInternal (id);
    if (id >= 128)
        noteOffInternal (id + 128);   // drugi glas terce
}

void SamplerPart::noteOffInternal (int id)
{
    const bool sustainOn = sustainDown && (cfg == nullptr || cfg->part.sustainPedal);
    const int slot = id / 128;

    for (int lane = 0; lane < numLanes; ++lane)
        laneNoteOff (lane, slot, id, sustainOn);

    const auto mask = polyMaskOf[(size_t) id];
    if (mask == 0)
        return;
    polyMaskOf[(size_t) id] = 0;
    if (id < 128 && polyHeld > 0)
        --polyHeld;

    for (auto& v : voices)
    {
        if (v.active && v.id == id && v.lane < 0 && v.keyDown && ! v.fadingOut)
        {
            v.keyDown = false;
            if (sustainOn) v.held = true;
            else releaseVoice (v);
        }
    }
    if (cfg != nullptr && cfg->active && slot == 0)
        startVoices (-1, idPitch[(size_t) id], idVel[(size_t) id], -1.0, false, mask, ++serialCounter, -1, 0, true);
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
    for (auto& row : lanes)
        for (auto& L : row) { L.held.clear(); L.key = -1; L.artic = -1; }
    polyMaskOf.fill (0);
    polyHeld = 0;
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
    if (num <= 0 || cfg == nullptr)
        return;

    auto* outL = partBuffer.getWritePointer (0) + start;
    auto* outR = partBuffer.getWritePointer (1) + start;
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
        const int endPos = loop ? len : juce::jlimit (1, len, v.endPos > 0 ? v.endPos : len);

        const int xf = loop ? v.loopXfade : 0;
        const int xfStart = loopEnd - xf;
        auto fetch = [&] (const float* d, int idx) -> float
        {
            if (loop && idx >= loopEnd) idx -= loopLen;
            if (idx < 0) idx = 0;
            if (xf > 0 && idx >= xfStart && idx < loopEnd)
            {
                // crossfade loop: kraj se pretapa u ono sto je pred pocetkom loop-a
                const float w = (float) (idx - xfStart) / (float) xf;
                return d[idx] * (1.0f - w) + d[idx - loopLen] * w;
            }
            return idx < endPos ? d[idx] : 0.0f;
        };

        for (int i = 0; i < num; ++i)
        {
            if (--v.ctrl <= 0)
            {
                v.ctrl = controlRate;
                updateModulation (v);
            }

            const int ip = (int) v.pos;
            const float t = (float) (v.pos - ip);
            float l = hermite (fetch (L, ip - 1), fetch (L, ip), fetch (L, ip + 1), fetch (L, ip + 2), t);
            float r = hermite (fetch (R, ip - 1), fetch (R, ip), fetch (R, ip + 1), fetch (R, ip + 2), t);

            float g = juce::jmax (0.0f, v.ampEnv.advance (1)) * v.velGain * v.ampMod;
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
            if (v.filterOn)
            {
                l = v.filter.processSample (0, l);
                r = v.filter.processSample (1, r);
                if (v.cascade)
                {
                    l = v.filter2.processSample (0, l);
                    r = v.filter2.processSample (1, r);
                }
            }
            if (v.eqOn)
            {
                l = v.eq[2].process (0, v.eq[1].process (0, v.eq[0].process (0, l)));
                r = v.eq[2].process (1, v.eq[1].process (1, v.eq[0].process (1, r)));
            }
            outL[i] += l;
            outR[i] += r;

            v.pos += v.inc;
            if (loop)
            {
                while (v.pos >= (double) loopEnd)
                    v.pos -= (double) loopLen;
            }
            else if (v.pos >= (double) (endPos - 1))
            {
                v.active = false;
                break;
            }

            if (! v.ampEnv.isActive())
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

        // EQ: user + doterivanje u performansu
        const float lo = juce::jlimit (-18.0f, 18.0f, u.eqLowDb + cfg->part.eqLowDb);
        const float mi = juce::jlimit (-18.0f, 18.0f, u.eqMidDb + cfg->part.eqMidDb);
        const float hi = juce::jlimit (-18.0f, 18.0f, u.eqHighDb + cfg->part.eqHighDb);
        const float mf = juce::jlimit (100.0f, 8000.0f, u.eqMidFreq);
        if (! juce::exactlyEqual (lo, eqLast[0])) { eqLow.set (Biquad::LowShelf, sampleRate, 150.0, 0.707, lo); eqLast[0] = lo; }
        if (! juce::exactlyEqual (mi, eqLast[1]) || ! juce::exactlyEqual (mf, eqLast[3]))
        { eqMid.set (Biquad::Peak, sampleRate, mf, 0.9, mi); eqLast[1] = mi; eqLast[3] = mf; }
        if (! juce::exactlyEqual (hi, eqLast[2])) { eqHigh.set (Biquad::HighShelf, sampleRate, 6000.0, 0.707, hi); eqLast[2] = hi; }
        if (std::abs (lo) > 0.05f || std::abs (mi) > 0.05f || std::abs (hi) > 0.05f)
        {
            auto* l = partBuffer.getWritePointer (0);
            auto* r = partBuffer.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                l[i] = eqHigh.process (0, eqMid.process (0, eqLow.process (0, l[i])));
                r[i] = eqHigh.process (1, eqMid.process (1, eqLow.process (1, r[i])));
            }
        }
    }

    const float rev = cfg != nullptr ? cfg->part.reverbSend * cfg->user.send1 : 0.0f;
    const float dly = cfg != nullptr ? cfg->part.delaySend * cfg->user.send2 : 0.0f;

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

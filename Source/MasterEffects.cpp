#include "MasterEffects.h"

void Biquad::set (Kind kind, double sampleRate, double freq, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / (2.0 * q);
    double B0, B1, B2, A0, A1, A2;

    switch (kind)
    {
        case LowShelf:
        {
            const double sq = 2.0 * std::sqrt (A) * alpha;
            B0 = A * ((A + 1) - (A - 1) * cw + sq);
            B1 = 2 * A * ((A - 1) - (A + 1) * cw);
            B2 = A * ((A + 1) - (A - 1) * cw - sq);
            A0 = (A + 1) + (A - 1) * cw + sq;
            A1 = -2 * ((A - 1) + (A + 1) * cw);
            A2 = (A + 1) + (A - 1) * cw - sq;
            break;
        }
        case HighShelf:
        {
            const double sq = 2.0 * std::sqrt (A) * alpha;
            B0 = A * ((A + 1) + (A - 1) * cw + sq);
            B1 = -2 * A * ((A - 1) + (A + 1) * cw);
            B2 = A * ((A + 1) + (A - 1) * cw - sq);
            A0 = (A + 1) - (A - 1) * cw + sq;
            A1 = 2 * ((A - 1) - (A + 1) * cw);
            A2 = (A + 1) - (A - 1) * cw - sq;
            break;
        }
        case Peak:
        default:
            B0 = 1 + alpha * A;
            B1 = -2 * cw;
            B2 = 1 - alpha * A;
            A0 = 1 + alpha / A;
            A1 = -2 * cw;
            A2 = 1 - alpha / A;
            break;
    }

    b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0;
    a1 = A1 / A0; a2 = A2 / A0;
}

//==============================================================================
void MasterEffects::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 2 };

    reverb.setSampleRate (sampleRate);
    reverb.reset();
    delay.prepare (spec);
    delay.reset();
    delayTime.reset (sampleRate, 0.1);
    delayTime.setCurrentAndTargetValue ((float) (0.35 * sampleRate));

    low.reset(); mid.reset(); high.reset();
    lastLow = lastMid = lastHigh = 999.0f;

    compressor.prepare (spec);
    compressor.setAttack (10.0f);
    compressor.setRelease (150.0f);
    limiterGain = 1.0f;
    limiterReleaseCoef = 1.0f - std::exp (-1.0f / (0.08f * (float) sampleRate));

    masterGain.reset (sampleRate, 0.05);
}

void MasterEffects::process (juce::AudioBuffer<float>& main,
                             juce::AudioBuffer<float>& revSend,
                             juce::AudioBuffer<float>& dlySend,
                             int n, const MasterSettings& s)
{
    auto* ml = main.getWritePointer (0);
    auto* mr = main.getWritePointer (1);

    // --- Reverb (send efekat) ---
    juce::Reverb::Parameters rp;
    rp.roomSize = s.reverbSize;
    rp.damping = s.reverbDamping;
    rp.wetLevel = 1.0f;
    rp.dryLevel = 0.0f;
    rp.width = 1.0f;
    reverb.setParameters (rp);
    reverb.processStereo (revSend.getWritePointer (0), revSend.getWritePointer (1), n);

    // --- Delay (send efekat) ---
    delayTime.setTargetValue ((float) (s.delayTimeMs * 0.001 * sr));
    auto* dl = dlySend.getWritePointer (0);
    auto* dr = dlySend.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float t = delayTime.getNextValue();
        const float yl = delay.popSample (0, t);
        const float yr = delay.popSample (1, t);
        delay.pushSample (0, dl[i] + yr * s.delayFeedback);
        delay.pushSample (1, dr[i] + yl * s.delayFeedback);
        dl[i] = yl;
        dr[i] = yr;
    }

    auto* rl = revSend.getReadPointer (0);
    auto* rr = revSend.getReadPointer (1);
    for (int i = 0; i < n; ++i)
    {
        ml[i] += rl[i] * s.reverbLevel + dl[i] * s.delayLevel;
        mr[i] += rr[i] * s.reverbLevel + dr[i] * s.delayLevel;
    }

    // --- EQ ---
    if (! juce::exactlyEqual (s.eqLowDb, lastLow))   { low.set  (Biquad::LowShelf,  sr, 120.0,  0.707, s.eqLowDb);  lastLow = s.eqLowDb; }
    if (! juce::exactlyEqual (s.eqMidDb, lastMid))   { mid.set  (Biquad::Peak,      sr, 1000.0, 0.8,   s.eqMidDb);  lastMid = s.eqMidDb; }
    if (! juce::exactlyEqual (s.eqHighDb, lastHigh)) { high.set (Biquad::HighShelf, sr, 7000.0, 0.707, s.eqHighDb); lastHigh = s.eqHighDb; }

    const bool eqOn = std::abs (s.eqLowDb) > 0.05f || std::abs (s.eqMidDb) > 0.05f || std::abs (s.eqHighDb) > 0.05f;
    if (eqOn)
    {
        for (int i = 0; i < n; ++i)
        {
            ml[i] = high.process (0, mid.process (0, low.process (0, ml[i])));
            mr[i] = high.process (1, mid.process (1, low.process (1, mr[i])));
        }
    }

    auto block = juce::dsp::AudioBlock<float> (main).getSubBlock (0, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> ctx (block);

    // --- Kompresor ---
    if (s.compRatio > 1.01f)
    {
        compressor.setThreshold (s.compThresholdDb);
        compressor.setRatio (s.compRatio);
        compressor.process (ctx);
    }

    // --- Master jacina + limiter (stiti zvucnike) ---
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (s.masterDb, -60.0f));
    const float ceiling = 0.944f; // -0.5 dBFS
    for (int i = 0; i < n; ++i)
    {
        const float g = masterGain.getNextValue();
        const float l = ml[i] * g;
        const float r = mr[i] * g;
        const float peak = juce::jmax (std::abs (l), std::abs (r));
        const float target = peak > ceiling ? ceiling / peak : 1.0f;
        if (target < limiterGain) limiterGain = target;                              // odmah stisa
        else                      limiterGain += (target - limiterGain) * limiterReleaseCoef; // polako pusti
        ml[i] = l * limiterGain;
        mr[i] = r * limiterGain;
    }
}

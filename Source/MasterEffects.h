#pragma once
#include <JuceHeader.h>

struct MasterSettings
{
    float reverbSize = 0.5f, reverbDamping = 0.5f, reverbLevel = 0.8f;
    float delayTimeMs = 350.0f, delayFeedback = 0.35f, delayLevel = 0.6f;
    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqHighDb = 0.0f;
    float compThresholdDb = 0.0f, compRatio = 1.0f;
    float masterDb = 0.0f;
};

// Jednostavan biquad bez alokacija na audio niti (RBJ formule)
class Biquad
{
public:
    enum Kind { LowShelf, Peak, HighShelf };
    void set (Kind kind, double sampleRate, double freq, double q, double gainDb);
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    inline float process (int ch, float x) noexcept
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }
private:
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};
};

class MasterEffects
{
public:
    void prepare (double sampleRate, int maxBlock);
    void process (juce::AudioBuffer<float>& main,
                  juce::AudioBuffer<float>& reverbSend,
                  juce::AudioBuffer<float>& delaySend,
                  int numSamples, const MasterSettings& s);

private:
    double sr = 44100.0;
    juce::Reverb reverb;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 192000 };
    juce::SmoothedValue<float> delayTime;
    Biquad low, mid, high;
    float lastLow = 999, lastMid = 999, lastHigh = 999;
    juce::dsp::Compressor<float> compressor;
    // providan limiter (ne menja jacinu ispod praga)
    float limiterGain = 1.0f, limiterReleaseCoef = 0.0f;
    juce::SmoothedValue<float> masterGain;
};

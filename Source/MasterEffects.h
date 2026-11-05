#pragma once
#include <JuceHeader.h>

struct MasterSettings
{
    float reverbSize = 0.5f, reverbDamping = 0.5f, reverbLevel = 0.8f;
    // plate reverb
    float reverbDecay = 2.2f;           // sekunde
    float reverbPreDelayMs = 20.0f;
    float reverbLowCut = 150.0f, reverbHighCut = 6000.0f;   // Hz, na ulazu u reverb
    float reverbDiffusion = 0.7f, reverbMod = 0.3f, reverbWidth = 1.0f;
    float delayTimeMs = 350.0f, delayFeedback = 0.35f, delayLevel = 0.6f;
    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqHighDb = 0.0f;
    float compThresholdDb = 0.0f, compRatio = 1.0f;
    float masterDb = 0.0f;
};

// MASTER EQ za ceo Workstation (ne zavisi od performansa)
// PARAMETRIC: low cut + bas (shelf) + 2 srednja (bell, frekvencija i sirina) + visoki (shelf)
// GRAPHIC: 15 opsega na 2/3 oktave (kao na miksetama za ozvucenje), 25 Hz - 16 kHz
struct GlobalEqSettings
{
    static constexpr int numGraphic = 15;
    static constexpr float graphicHz[numGraphic] { 25, 40, 63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000 };
    bool on = true;
    int mode = 0;                       // 0 parametric, 1 graphic
    float hpfHz = 20.0f;                // low cut (20 = iskljuceno)
    float lowDb = 0.0f, lowHz = 100.0f;
    float m1Db = 0.0f, m1Hz = 400.0f, m1Q = 1.0f;
    float m2Db = 0.0f, m2Hz = 2500.0f, m2Q = 1.0f;
    float highDb = 0.0f, highHz = 8000.0f;
    std::array<float, numGraphic> g {};
    bool active() const
    {
        if (! on) return false;
        if (mode == 1) { for (auto x : g) if (std::abs (x) > 0.05f) return true; return false; }
        return hpfHz > 21.0f || std::abs (lowDb) > 0.05f || std::abs (m1Db) > 0.05f || std::abs (m2Db) > 0.05f || std::abs (highDb) > 0.05f;
    }
    bool operator!= (const GlobalEqSettings& o) const
    {
        return on != o.on || mode != o.mode || hpfHz != o.hpfHz || lowDb != o.lowDb || lowHz != o.lowHz || m1Db != o.m1Db || m1Hz != o.m1Hz || m1Q != o.m1Q
            || m2Db != o.m2Db || m2Hz != o.m2Hz || m2Q != o.m2Q || highDb != o.highDb || highHz != o.highHz || g != o.g;
    }
    // jedan parametar po imenu (iPad, cuvanje): lowDb, m1Hz, g7...
    bool setByName (const juce::String& k, float v);
    float getByName (const juce::String& k) const;
    static juce::StringArray names();
};

// Jednostavan biquad bez alokacija na audio niti (RBJ formule)
class Biquad
{
public:
    enum Kind { LowShelf, Peak, HighShelf, HighPass };
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

// Plate reverb (Dattorro): mekan, gust rep bez "pucketavih" ranih refleksija
class PlateReverb
{
public:
    void prepare (double sampleRate);
    void reset();
    // ulaz: stereo send (sabira se u mono), izlaz se upisuje preko ulaza
    void process (float* l, float* r, int n, const MasterSettings& s);

private:
    struct Line
    {
        std::vector<float> buf;
        int w = 0;
        void init (int len) { buf.assign ((size_t) juce::jmax (4, len + 4), 0.0f); w = 0; }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
        inline void write (float x) noexcept { buf[(size_t) w] = x; if (++w >= (int) buf.size()) w = 0; }
        inline float read (int d) const noexcept
        {
            int i = w - 1 - d;
            while (i < 0) i += (int) buf.size();
            return buf[(size_t) i];
        }
        inline float readFrac (float d) const noexcept
        {
            const int di = (int) d;
            const float f = d - (float) di;
            return read (di) * (1.0f - f) + read (di + 1) * f;
        }
    };
    struct Allpass
    {
        Line line; int len = 1;
        inline float process (float x, float g) noexcept
        {
            const float d = line.read (len - 1);
            const float v = x - g * d;
            line.write (v);
            return d + g * v;
        }
    };

    double sr = 44100.0;
    float scale = 1.0f, lastSize = -1.0f;
    Line preDelay;
    Allpass inAp[4];
    Line tankAp1[2];           // modulisani allpass
    int tankAp1Len[2] {};
    Line tankDelay1[2], tankDelay2[2];
    int tankDelay1Len[2] {}, tankDelay2Len[2] {};
    Allpass tankAp2[2];
    float damp[2] {}, tankFeed[2] {};
    float bwState = 0.0f, hpState = 0.0f, hpPrev = 0.0f;
    double lfoPhase = 0.0;
    void build (float size);
    int ms (int samples29k) const { return juce::jmax (2, (int) std::round (samples29k * scale)); }
};

class MasterEffects
{
public:
    void prepare (double sampleRate, int maxBlock);
    void process (juce::AudioBuffer<float>& main,
                  juce::AudioBuffer<float>& reverbSend,
                  juce::AudioBuffer<float>& delaySend,
                  int numSamples, const MasterSettings& s, const GlobalEqSettings& g = {});

private:
    double sr = 44100.0;
    PlateReverb reverb;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 192000 };
    juce::SmoothedValue<float> delayTime;
    Biquad low, mid, high;
    Biquad gHpf, gLow, gM1, gM2, gHigh;
    std::array<Biquad, GlobalEqSettings::numGraphic> gBand;
    GlobalEqSettings gLast; bool gInit = false;
    float lastLow = 999, lastMid = 999, lastHigh = 999;
    juce::dsp::Compressor<float> compressor;
    // providan limiter (ne menja jacinu ispod praga)
    float limiterGain = 1.0f, limiterReleaseCoef = 0.0f;
    juce::SmoothedValue<float> masterGain;
};

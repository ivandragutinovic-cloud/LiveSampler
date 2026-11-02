#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"
#include "Model.h"
#include "MasterEffects.h"

// Sve sto deo performansa treba za sviranje (nepromenljivo dok traje jedan audio blok)
struct PartRuntime
{
    PerfPart part;
    UserProgram user;
    std::array<InstrumentPtr, kNumOsc> inst;
    bool active = false;   // ukljucen i ima usera
};

//==============================================================================
// Insert efekat jednog dela
class InsertFx
{
public:
    enum Type { Off = 0, Chorus, Phaser, Tremolo, Overdrive, Echo, NumTypes };
    static juce::StringArray getTypeNames()
    {
        return { "Bez efekta", "Chorus", "Phaser", "Tremolo", "Overdrive", "Eho (delay)" };
    }

    void prepare (double sampleRate, int maxBlock);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, int type, float rate, float depth, float mix);

private:
    double sr = 44100.0;
    int lastType = -1;
    juce::AudioBuffer<float> dry;
    juce::dsp::Chorus<float> chorus;
    juce::dsp::Phaser<float> phaser;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 192000 };
    juce::SmoothedValue<float> delayTimeSmoothed;
    double tremPhase = 0.0;
    float odLowpassState[2] {};
};

//==============================================================================
// Envelope kao na Kronosu (linearni segmenti izmedju nivoa)
class KronosEnv
{
public:
    enum Stage { Idle = 0, Attack, Decay, Slope, Sustain, Release };

    void start (const EnvSettings& e, double sampleRate);
    void release();
    void kill() { stage = Idle; level = 0.0f; }
    // napreduje n semplova, vraca nivo
    float advance (int n);
    float getLevel() const noexcept { return level; }
    bool isActive() const noexcept { return stage != Idle; }
    Stage getStage() const noexcept { return stage; }

private:
    void enter (Stage s, float target, float seconds);

    EnvSettings env;
    double sr = 44100.0;
    Stage stage = Idle;
    float level = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0;
};

//==============================================================================
// Jedan deo performansa: user sa do 8 oscilatora (Zvuk A/B), insert efekat
class SamplerPart
{
public:
    void prepare (double sampleRate, int maxBlock);

    // audio nit
    void beginBlock (const PartRuntime* runtime, int numSamples);
    void noteOn (int id, int pitch, int velocity, bool additive = false);   // id: 0-127 taster, 128+ terca
    void noteOff (int id);
    void setSustain (bool down);
    void setPitchBend (float minus1to1) { bend = minus1to1; }
    void setModWheel (float v) { modWheel = v; }
    void setAftertouch (float v) { aftertouch = v; }
    void allNotesOff (bool immediate);
    void renderVoices (int startSample, int numSamples);
    void endBlock (juce::AudioBuffer<float>& mainOut, juce::AudioBuffer<float>& reverbSend,
                   juce::AudioBuffer<float>& delaySend, int numSamples);

    int getNumActiveVoices() const;
    static constexpr int numIds = 256;
    static constexpr int controlRate = 16;   // modulacija se racuna na svakih 16 semplova

private:
    struct Voice
    {
        bool active = false;
        int id = -1;
        juce::uint32 serial = 0;
        bool keyDown = false, held = false;
        InstrumentPtr instrument;
        const SampleData* sample = nullptr;
        int block = 0;
        int oscTranspose = 0;
        double pos = 0.0, inc = 1.0;
        double pitch = 60.0, pitchTarget = 60.0, glideCoef16 = 0.0;
        double rootKey = 60.0, tune = 0.0;     // tune u polutonovima
        bool fixedPitch = false, loop = false;
        int loopStart = 0, loopEnd = 0, endPos = 0;
        float vel01 = 1.0f, velGain = 1.0f, gainL = 1.0f, gainR = 1.0f, ampMod = 1.0f;
        bool fadingOut = false;
        float fadeGain = 1.0f;
        juce::uint64 age = 0;
        KronosEnv ampEnv, pitchEnv, filterEnv;
        double vibPhase = 0.0, fltPhase = 0.0, time = 0.0;
        int ctrl = 0;
        bool filterOn = false, cascade = false;
        juce::dsp::StateVariableTPTFilter<float> filter, filter2;
    };

    Voice* findFreeVoice();
    void startVoices (int id, int pitch, int velocity, double startPitch, bool glide);
    void releaseVoice (Voice& v);
    void updateModulation (Voice& v);
    void setGlide (Voice& v, double fromPitch, bool glide);
    int chooseMonoKey() const;
    void soundMono (int id, bool transition);

    static constexpr int maxVoices = 128;
    std::array<Voice, maxVoices> voices;

    const PartRuntime* cfg = nullptr;
    double sampleRate = 44100.0;
    juce::uint64 ageCounter = 0;
    juce::uint32 serialCounter = 0, monoSerial = 0;
    int monoKey = -1;
    bool sustainDown = false;
    float bend = 0.0f, modWheel = 0.0f, aftertouch = 0.0f;
    double lastPitch = -1.0;
    int polyHeld = 0;
    std::vector<int> heldKeys;                 // mono
    std::array<int, numIds> idPitch {}, idVel {};

    juce::AudioBuffer<float> partBuffer;
    InsertFx insertFx;
    Biquad eqLow, eqMid, eqHigh;
    float eqLast[4] { 999.0f, 999.0f, 999.0f, 999.0f };
    juce::SmoothedValue<float> gainL, gainR;
};

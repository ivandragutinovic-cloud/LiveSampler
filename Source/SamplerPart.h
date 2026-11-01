#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"
#include "Model.h"

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
// Jedan deo performansa: user sa do 8 oscilatora, zajednicki ADSR/filter, insert efekat
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
    void allNotesOff (bool immediate);
    void renderVoices (int startSample, int numSamples);
    void endBlock (juce::AudioBuffer<float>& mainOut, juce::AudioBuffer<float>& reverbSend,
                   juce::AudioBuffer<float>& delaySend, int numSamples);

    int getNumActiveVoices() const;
    static constexpr int numIds = 256;

private:
    struct Voice
    {
        bool active = false;
        int id = -1;
        juce::uint32 serial = 0;
        bool keyDown = false, held = false;
        InstrumentPtr instrument;
        const SampleData* sample = nullptr;
        int oscTranspose = 0;
        double pos = 0.0;
        double pitch = 60.0, pitchTarget = 60.0;
        double rootKey = 60.0, tune = 0.0;     // tune u polutonovima
        bool fixedPitch = false, loop = false;
        int loopStart = 0, loopEnd = 0;
        float velGain = 1.0f, gainL = 1.0f, gainR = 1.0f;
        bool fadingOut = false;
        float fadeGain = 1.0f;
        juce::uint64 age = 0;
        juce::ADSR env;
        juce::dsp::StateVariableTPTFilter<float> filter;
    };

    Voice* findFreeVoice();
    void startVoices (int id, int pitch, int velocity, double startPitch);
    void releaseVoice (Voice& v);

    static constexpr int maxVoices = 128;
    std::array<Voice, maxVoices> voices;

    const PartRuntime* cfg = nullptr;
    double sampleRate = 44100.0;
    juce::uint64 ageCounter = 0;
    juce::uint32 serialCounter = 0, monoSerial = 0;
    bool sustainDown = false;
    float bend = 0.0f;
    double lastPitch = -1.0;
    std::vector<int> heldKeys;                 // mono
    std::array<int, numIds> idPitch {};
    juce::ADSR::Parameters envParams;
    double glideCoef = 0.0;
    bool filterOn = false;
    float filterQ = 0.7071f;

    juce::AudioBuffer<float> partBuffer;
    InsertFx insertFx;
    juce::SmoothedValue<float> gainL, gainR;
};

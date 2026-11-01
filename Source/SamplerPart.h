#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"

// Sva podesavanja jednog dela (Gornji 1, Gornji 2, Donji), citaju se jednom po bloku
struct PartSettings
{
    bool  enabled = true;
    float volumeDb = 0.0f, pan = 0.0f;
    int   octave = 0, transpose = 0;
    float fineCents = 0.0f;
    float velSens = 1.0f;          // 0 = uvek ista jacina, 1 = puna osetljivost
    bool  mono = false;
    float portamento = 0.0f;       // sekunde
    float attack = 0.001f, decay = 0.3f, sustain = 1.0f, release = 0.3f;
    float cutoff = 20000.0f, resonance = 0.0f;
    int   fxType = 0;
    float fxRate = 0.3f, fxDepth = 0.5f, fxMix = 0.5f;
    float reverbSend = 0.2f, delaySend = 0.0f;
    int   bendRange = 2;
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
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const PartSettings& s);

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
class SamplerPart
{
public:
    SamplerPart();

    void prepare (double sampleRate, int maxBlock);

    // poziva se sa message niti
    void setInstrument (InstrumentPtr newInstrument);
    InstrumentPtr getInstrument() const;

    // poziva audio nit
    void beginBlock (const PartSettings& settings, int numSamples);
    void noteOn (int note, int velocity);
    void noteOff (int note);
    void setSustain (bool down);
    void setPitchBend (float minus1to1) { bend = minus1to1; }
    void allNotesOff (bool immediate);
    void renderVoices (int startSample, int numSamples);
    void endBlock (juce::AudioBuffer<float>& mainOut,
                   juce::AudioBuffer<float>& reverbSend,
                   juce::AudioBuffer<float>& delaySend, int numSamples);

    int getNumActiveVoices() const;

private:
    struct Voice
    {
        bool active = false;
        int key = -1;                // taster koji je pokrenuo glas
        bool keyDown = false, held = false;
        InstrumentPtr instrument;    // drzi instrument u memoriji dok glas svira
        const SampleData* sample = nullptr;
        double pos = 0.0;
        double pitch = 60.0, pitchTarget = 60.0;
        double rootKey = 60.0, zoneTune = 0.0;   // u polutonovima
        bool fixedPitch = false, loop = false;
        int loopStart = 0, loopEnd = 0;
        float velGain = 1.0f, zoneGainL = 1.0f, zoneGainR = 1.0f;
        bool fadingOut = false;
        float fadeGain = 1.0f;
        juce::uint64 age = 0;
        juce::ADSR env;
        juce::dsp::StateVariableTPTFilter<float> filter;
    };

    Voice* findFreeVoice();
    void startVoice (Voice& v, int key, int velocity, double startPitch, double targetPitch);
    void releaseVoice (Voice& v);
    double keyToPitch (int key) const;

    static constexpr int maxVoices = 96;
    std::array<Voice, maxVoices> voices;

    juce::SpinLock instrumentLock;
    InstrumentPtr instrument;        // menja message nit
    InstrumentPtr blockInstrument;   // kopija za tekuci blok

    PartSettings s;
    double sampleRate = 44100.0;
    juce::uint64 ageCounter = 0;
    bool sustainDown = false;
    float bend = 0.0f;
    double lastPitch = -1.0;
    std::vector<int> heldKeys;       // za mono rezim
    juce::ADSR::Parameters envParams;
    double glideCoef = 0.0;
    bool filterOn = false;

    juce::AudioBuffer<float> partBuffer;
    InsertFx insertFx;
    juce::SmoothedValue<float> gainL, gainR;
};

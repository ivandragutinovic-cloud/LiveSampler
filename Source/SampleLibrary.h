#pragma once
#include <JuceHeader.h>

// Jedan WAV fajl ucitan u memoriju
struct SampleData
{
    juce::AudioBuffer<float> buffer;   // uvek 2 kanala
    double sampleRate = 44100.0;
    int rootNote = 60;
    int startOffset = 0;               // preskace tisinu na pocetku
    bool hasLoop = false;
    int loopStart = 0, loopEnd = 0;    // loopEnd je ekskluzivan
    juce::String fileName;
};

// Oblast na klavijaturi + jacina udarca koja svira odredjene semplove
struct Zone
{
    int lowKey = 0, highKey = 127;
    int lowVel = 1, highVel = 127;
    std::vector<int> sampleIndices;    // vise od jednog = round robin
};

class Instrument
{
public:
    juce::String name;
    std::vector<std::unique_ptr<SampleData>> samples;
    std::vector<Zone> zones;
    juce::StringArray warnings;

    const SampleData* findSample (int note, int velocity) const;
    size_t memoryBytes() const;

private:
    mutable std::atomic<unsigned> roundRobinCounter { 0 };
};

using InstrumentPtr = std::shared_ptr<Instrument>;

class SampleLibrary
{
public:
    SampleLibrary();

    // Pretrazuje folder: svaki podfolder sa audio fajlovima postaje instrument
    void scan (const juce::File& rootFolder);

    const juce::StringArray& getInstrumentNames() const { return names; }
    juce::File getRootFolder() const { return root; }

    // Ucitava instrument (poziva se iz pozadinske niti, moze da traje)
    InstrumentPtr loadInstrument (const juce::String& name, bool c3Is60);

    // Iz naziva fajla vadi notu i velocity sloj. Vraca false ako note nema.
    static bool parseFileName (const juce::String& fileNameWithoutExt, bool c3Is60,
                               int& note, int& velocityLayer);

private:
    juce::File root;
    juce::StringArray names;
    std::map<juce::String, juce::File> folders;
    juce::AudioFormatManager formatManager;
    juce::CriticalSection lock;
};

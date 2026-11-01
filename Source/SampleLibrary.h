#pragma once
#include <JuceHeader.h>

// Jedan sempl ucitan u memoriju (deli se izmedju verzija instrumenta)
struct SampleData
{
    juce::AudioBuffer<float> buffer;   // uvek 2 kanala
    double sampleRate = 44100.0;
    int startOffset = 0;               // preskace tisinu na pocetku
    bool hasLoop = false;
    int loopStart = 0, loopEnd = 0;    // loopEnd je ekskluzivan
    int fileRootKey = -1;              // root iz WAV smpl bloka, ako postoji
    juce::String id;                   // relativna putanja, koristi se u fajlu mape
    juce::String displayName;

    double lengthSeconds() const { return buffer.getNumSamples() / sampleRate; }
};

using SamplePtr = std::shared_ptr<const SampleData>;

// Zona: koji sempl svira na kojim tasterima i jacinama
struct Zone
{
    int sample = -1;
    bool enabled = true;
    int lowKey = 0, highKey = 127;
    int lowVel = 1, highVel = 127;
    int rootKey = 60;
    float tuneCents = 0.0f;
    float gainDb = 0.0f;
    float pan = 0.0f;                  // -1 .. 1
    bool loop = true;                  // koristi loop (zone ili sempla, ako postoji)
    bool fixedPitch = false;           // ista visina na svim tasterima (bubnjevi)
    int sampleStart = 0;               // > 0: pocetak sempla (inace automatski bez tisine)
    int loopStart = -1, loopEnd = -1;  // loop zone (Kontakt), ima prednost nad loop-om sempla

    bool hasOwnLoop() const noexcept   { return loopStart >= 0 && loopEnd > loopStart + 16; }

    bool matches (int key, int vel) const noexcept
    {
        return enabled && key >= lowKey && key <= highKey && vel >= lowVel && vel <= highVel;
    }
};

enum class SourceType { WavFolder, Korg, Kontakt };

class Instrument
{
public:
    juce::String name;
    SourceType type = SourceType::WavFolder;
    juce::File source;                 // folder (WAV) ili .kmp / .nki fajl
    std::vector<SamplePtr> samples;
    std::vector<Zone> zones;
    juce::StringArray warnings;
    bool fromMapFile = false;

    const Zone* findZone (int key, int velocity) const;
    size_t memoryBytes() const;
    juce::File getMapFile() const;

    // nova verzija sa drugim zonama (semplovi se dele)
    std::shared_ptr<Instrument> withZones (std::vector<Zone> newZones) const;

    bool saveMap() const;

private:
    mutable std::atomic<unsigned> roundRobinCounter { 0 };
};

using InstrumentPtr = std::shared_ptr<Instrument>;

juce::String sourceTypeName (SourceType t);

class SampleLibrary
{
public:
    SampleLibrary();

    // Pretrazuje foldere (disk, USB): WAV folderi, Korg .kmp i Kontakt .nki postaju instrumenti.
    // Ime instrumenta pocinje imenom foldera biblioteke, npr. "Kontakt Library/Gudaci/Violina"
    void scan (const juce::Array<juce::File>& rootFolders);
    void scan (const juce::File& rootFolder) { scan (juce::Array<juce::File> { rootFolder }); }

    const juce::StringArray& getInstrumentNames() const { return names; }
    bool contains (const juce::String& name) const { const juce::ScopedLock sl (lock); return entries.count (name) > 0; }
    static juce::String rootLabel (const juce::File& root);

    // Ucitava instrument (poziva se iz pozadinske niti, moze da traje).
    // ignoreMapFile: ne koristi sacuvanu mapu (za "vrati automatsko")
    InstrumentPtr loadInstrument (const juce::String& name, bool c3Is60, bool ignoreMapFile = false);

    // Iz naziva fajla vadi notu i velocity sloj. Vraca false ako note nema.
    static bool parseFileName (const juce::String& fileNameWithoutExt, bool c3Is60,
                               int& note, int& velocityLayer);

    // Korg i Kontakt citaci (javni zbog testova)
    void loadKorg (Instrument& inst);
    void loadKontakt (Instrument& inst, bool c3Is60);
    SamplePtr loadKsf (const juce::File& f, const juce::String& id, juce::StringArray& warnings);
    SamplePtr loadAudioFile (const juce::File& f, const juce::String& id, juce::StringArray& warnings);

    static void autoMapByName (Instrument& inst, const std::vector<int>& sampleIndices, bool c3Is60);
    static bool applyMapFile (Instrument& inst);

private:
    struct Entry { SourceType type; juce::File file; };

    void loadWavFolder (Instrument& inst, bool c3Is60);
    void scanRoot (const juce::File& root);

    juce::Array<juce::File> roots;
    juce::StringArray names;
    std::map<juce::String, Entry> entries;
    juce::AudioFormatManager formatManager;
    juce::CriticalSection lock;
};

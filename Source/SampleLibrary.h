#pragma once
#include <JuceHeader.h>

// Jedan sempl ucitan u memoriju (deli se izmedju verzija instrumenta)
struct SampleData
{
    // Dok se ucitava: float bafer (2 kanala). Posle compact() zvuk je u 16-bit PCM (mono kad su kanali isti),
    // pa zauzima 2-4 puta manje RAM-a. Alati (MAPPING, auto loop) dobijaju float kopiju preko audio().
    juce::AudioBuffer<float> buffer;
    std::shared_ptr<const std::vector<juce::int16>> pcmL, pcmR;   // pcmR prazan = mono
    int numFrames = 0;
    float pcmScale = 1.0f / 32768.0f;                             // vrednost = pcm * pcmScale
    double sampleRate = 44100.0;
    int startOffset = 0;               // preskace tisinu na pocetku
    bool hasLoop = false;
    int loopStart = 0, loopEnd = 0;    // loopEnd je ekskluzivan
    int endPoint = 0;                  // kraj sempla bez loop-a (ekskluzivan), 0 = do kraja bafera
    int fileRootKey = -1;              // root iz WAV smpl bloka, ako postoji
    juce::String id;                   // relativna putanja, koristi se u fajlu mape
    juce::String displayName;

    // citanje sa diska: u RAM-u je samo prvih residentFrames() frejmova, ostalo cita DiskStreamer iz fajla
    bool streamed = false;
    juce::File file;
    int residentFrames() const { return pcmL != nullptr ? (int) pcmL->size() : buffer.getNumSamples(); }

    void compact (int keepFrames = -1);               // float -> 16-bit (posle ucitavanja); keepFrames > 0: samo pocetak u RAM-u
    bool isCompact() const { return pcmL != nullptr; }
    bool isMono() const { return pcmL != nullptr && pcmR == nullptr; }
    int length() const { return pcmL != nullptr ? numFrames : buffer.getNumSamples(); }
    const juce::AudioBuffer<float>& audio() const;   // float kopija za alate (samo message nit)
    size_t memoryBytes() const;
    double lengthSeconds() const { return length() / sampleRate; }

private:
    struct FloatCache { juce::CriticalSection lock; std::unique_ptr<juce::AudioBuffer<float>> buf; };
    std::shared_ptr<FloatCache> cache { std::make_shared<FloatCache>() };
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
    int loopStart = -1, loopEnd = -1;  // loop zone (Kontakt / editor), ima prednost nad loop-om sempla
    int sampleEnd = -1;                // > 0: kraj sempla kad nema loop-a (editor)
    int loopXfade = 0;                 // crossfade pred krajem loop-a (semplovi)

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
    // round robin: vise ukljucenih zona na istom tasteru i jacini sviraju naizmenicno
    // false = redom (1, 2, 3, 1...), true = nasumicno (nikad isti dvaput zaredom)
    bool rrRandom = false;

    const Zone* findZone (int key, int velocity) const;
    int roundRobinCount (int key, int velocity) const;   // koliko zona deli ovo mesto (1 = nema round robin)
    size_t memoryBytes() const;
    juce::File getMapFile() const;

    // nova verzija sa drugim zonama (semplovi se dele)
    std::shared_ptr<Instrument> withZones (std::vector<Zone> newZones) const;

    bool saveMap() const;

private:
    mutable std::atomic<unsigned> roundRobinCounter { 0 };
    mutable std::array<std::atomic<unsigned char>, 128> rrNext {};   // po tasteru: sledeci (redom) / poslednji (nasumicno)
    mutable std::atomic<juce::uint32> rrSeed { 12345u };
};

using InstrumentPtr = std::shared_ptr<Instrument>;

juce::String sourceTypeName (SourceType t);

class SampleLibrary
{
public:
    // citanje sa diska (SETTINGS): semplovi duzi od streamMinSec drze u RAM-u samo streamPreloadSec sa pocetka
    static std::atomic<bool> streamingEnabled;
    static constexpr double streamMinSec = 2.5, streamPreloadSec = 0.75;
    // zone koje traze loop / start posle preload-a: taj sempl se ipak ucita ceo (vraca true ako je nesto promenjeno)
    static bool makeZonesPlayable (Instrument&);
    static SamplePtr loadResident (const SampleData&);
    SampleLibrary();

    // Pretrazuje foldere (disk, USB): WAV folderi, Korg .kmp i Kontakt .nki postaju instrumenti.
    // Ime instrumenta pocinje imenom foldera biblioteke, npr. "Kontakt Library/Gudaci/Violina"
    void scan (const juce::Array<juce::File>& rootFolders);
    void scan (const juce::File& rootFolder) { scan (juce::Array<juce::File> { rootFolder }); }

    const juce::StringArray& getInstrumentNames() const { return names; }
    bool contains (const juce::String& name) const { const juce::ScopedLock sl (lock); return entries.count (name) > 0; }
    // folder / fajl instrumenta na disku (prazno ako ga nema)
    juce::File instrumentFile (const juce::String& name) const { const juce::ScopedLock sl (lock); auto it = entries.find (name); return it != entries.end() ? it->second.file : juce::File(); }
    static juce::String rootLabel (const juce::File& root);

    // Kronos: instrument za multisempl broj 'index' iz KSC kolekcije sa datim UUID-om
    juce::String findKronosMultisample (const juce::String& uuid, int index) const;
    juce::String findKronosRom (const juce::String& romKey) const;
    juce::String kronosTag (const juce::String& key, const juce::String& name) const;   // npr. "release" za presemplovan EXi

    // Ucitava instrument (poziva se iz pozadinske niti, moze da traje).
    // ignoreMapFile: ne koristi sacuvanu mapu (za "vrati automatsko")
    InstrumentPtr loadInstrument (const juce::String& name, bool c3Is60, bool ignoreMapFile = false);

    // Iz naziva fajla vadi notu i velocity sloj. Vraca false ako note nema.
    static bool parseFileName (const juce::String& fileNameWithoutExt, bool c3Is60,
                               int& note, int& velocityLayer);

    // Korg i Kontakt citaci (javni zbog testova)
    void loadKorg (Instrument& inst);
    void loadKontakt (Instrument& inst, bool c3Is60);
    SamplePtr loadKsf (const juce::File& f, const juce::String& id, juce::StringArray& warnings, int depth = 0);
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
    std::map<juce::String, juce::String> kronosMs;   // "uuid:broj" -> ime instrumenta
    std::map<juce::String, juce::StringPairArray> kronosTags;   // "exi:..." -> release=, layers= (iz kronos-ms.txt)
    juce::AudioFormatManager formatManager;
    juce::CriticalSection lock;
};

#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"
#include "SamplerPart.h"
#include "MasterEffects.h"
#include "Model.h"
#include "KronosPcg.h"

class LiveSamplerProcessor : public juce::AudioProcessor,
                             public juce::ChangeBroadcaster,
                             private juce::AsyncUpdater,
                             private juce::Timer
{
public:
    static juce::Colour partColour (int i);

    LiveSamplerProcessor();
    ~LiveSamplerProcessor() override;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Live Sampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::MidiKeyboardState keyboardState;

    //==========================================================================
    // Biblioteka (folderi sa semplovima, USB)
    juce::Array<juce::File> getLibraryFolders() const { return libraryFolders; }
    void addLibraryFolder (const juce::File& folder);
    void removeLibraryFolder (int index);
    void rescanLibrary();
    const juce::StringArray& getInstrumentList() const { return instrumentList; }
    bool isScanning() const { return scanning; }
    bool getC3Is60() const { return c3Is60; }
    void setC3Is60 (bool shouldBe);

    // Useri
    int getNumUsers() const { return (int) users.size(); }
    const UserProgram& getUser (int i) const { return users[(size_t) juce::jlimit (0, juce::jmax (0, (int) users.size() - 1), i)]; }
    int findUser (const juce::String& name) const;
    juce::StringArray getUserNames() const;
    int createUser (const juce::String& baseName, const UserProgram* copyFrom = nullptr);
    int createUserFromInstrument (const juce::String& instrument);
    void updateUser (int index, const UserProgram& u);         // promena se cuje odmah
    void deleteUser (int index);
    juce::String uniqueUserName (const juce::String& base, int ignoreIndex = -1) const;

    // Performans (radna kopija) i 64 mesta
    const Performance& getPerformance() const { return perf; }
    void updatePerformance (const Performance& p);
    const Performance& getSlot (int i) const { return slots[(size_t) juce::jlimit (0, kNumPerfSlots - 1, i)]; }
    void storePerformance (int slot);
    void recallPerformance (int slot);
    int getCurrentSlot() const { return currentSlot; }
    bool isPerformanceDirty() const { return perfDirty; }
    int getCurrentBank() const { return currentBank; }
    void setCurrentBank (int b);

    // Terce
    const TercaSettings& getTerca() const { return terca; }
    void updateTerca (const TercaSettings& t);
    bool isTercaOn() const { return tercaOn.load(); }
    int getTercaRoot() const { return tercaRoot.load(); }
    int getTercaScale() const { return tercaScale.load(); }
    void setTercaOn (bool on);
    void setTercaKey (int root, int scale);
    void tercaButton (bool down);                               // dugme TERCA na ekranu
    void startLearn (bool shouldLearn) { learning = shouldLearn; sendChangeMessage(); }
    bool isLearning() const { return learning.load(); }
    juce::String getTercaText() const;

    // Editor mapiranja
    InstrumentPtr getLoadedInstrument (const juce::String& name) const;
    void applyZones (const juce::String& name, std::vector<Zone> zones);
    bool saveMap (const juce::String& name);
    void reloadInstrument (const juce::String& name, bool ignoreMapFile);
    void ensureLoaded (const juce::String& name) { if (instrumentList.contains (name) && cache.count (name) == 0) requestInstrument (name, nullptr); }

    // Korg Kronos
    bool loadKronosPcg (const juce::File& f, juce::String& error);
    const Kronos::Pcg* getKronosPcg() const { return kronosPcg.get(); }
    juce::File getKronosFile() const { return kronosFile; }
    juce::String findKronosMultisample (const juce::String& uuid, int ms) const { return library.findKronosMultisample (uuid, ms); }
    int createUserFromKronos (const Kronos::Program& p);   // -1 ako nema user semplova u biblioteci

    float getGlobalVolume() const { return globalDb; }
    void setGlobalVolume (float db) { globalDb = db; rebuildConfig(); markSave(); }

    juce::String getStatusText() const;
    int getActiveVoices() const { return activeVoices.load(); }
    void panic();

    juce::File getDataFile() const;
    void saveNow();

private:
    struct EngineConfig
    {
        std::array<PartRuntime, kNumParts> parts;
        MasterSettings master;
        float globalDb = 0.0f;
        TercaSettings terca;
    };
    using ConfigPtr = std::shared_ptr<const EngineConfig>;

    void handleAsyncUpdate() override;
    void timerCallback() override;

    void rebuildConfig();
    void loadNeededInstruments();
    juce::StringArray neededInstruments (bool includeBank) const;
    void markSave() { saveNeeded = true; }
    void loadData();
    void bootstrapIfEmpty();

    using LoadCallback = std::function<void (InstrumentPtr)>;
    void requestInstrument (const juce::String& name, LoadCallback cb, bool ignoreMapFile = false);
    void onInstrumentLoaded (const juce::String& name, InstrumentPtr inst, int epoch);
    void installInstrument (const juce::String& name, InstrumentPtr inst);

    // audio nit
    void handleMidi (const juce::MidiMessage& m, const EngineConfig& c);
    void routeNoteOn (int ch, int note, int vel, const EngineConfig& c);
    void routeNoteOff (int ch, int note);
    void tercaTrigger (bool down, const EngineConfig& c);
    void tryRecognize (int mask, const EngineConfig& c);
    int heldMask() const;

    // audio
    SamplerPart parts[kNumParts];
    MasterEffects master;
    juce::AudioBuffer<float> mainBuf, reverbBuf, delayBuf;
    int maxBlock = 512;
    juce::uint8 routed[16][128] {};
    juce::uint8 harmonized[16][128] {};
    int heldCount[128] {};
    bool tercaCapturing = false, recognizedInCapture = false;
    int capturedMask = 0;

    juce::SpinLock configLock;
    ConfigPtr pendingConfig;      // poslednja verzija (message nit)
    ConfigPtr audioConfig;        // koju audio nit koristi
    std::vector<ConfigPtr> configGraveyard;

    std::atomic<int> pendingProgram { -1 };
    std::atomic<bool> panicRequested { false };
    std::atomic<bool> tercaOn { false };
    std::atomic<int> tercaRoot { 2 }, tercaScale { 1 };
    std::atomic<bool> learning { false };
    std::atomic<int> learnedType { 0 }, learnedChannel { 0 }, learnedNumber { 0 };
    std::atomic<bool> tercaChanged { false };
    std::atomic<int> activeVoices { 0 };
    juce::AbstractFifo uiTriggerFifo { 32 };
    int uiTriggerData[32] {};

    // biblioteka i ucitavanje
    SampleLibrary library;
    juce::Array<juce::File> libraryFolders;
    juce::StringArray instrumentList;
    bool c3Is60 = true;
    bool scanning = false;
    std::atomic<int> scanCounter { 0 };
    int loadEpoch = 0;
    std::map<juce::String, InstrumentPtr> cache;
    std::map<juce::String, std::vector<LoadCallback>> pendingLoads;
    std::vector<InstrumentPtr> graveyard;
    juce::String lastWarning;
    juce::ThreadPool loadPool { 1 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    std::unique_ptr<Kronos::Pcg> kronosPcg;
    juce::File kronosFile;

    // korisnicki podaci
    std::vector<UserProgram> users;
    std::array<Performance, kNumPerfSlots> slots;
    Performance perf;
    int currentSlot = -1, currentBank = 0;
    bool perfDirty = false;
    TercaSettings terca;
    float globalDb = 0.0f;
    bool saveNeeded = false;
    juce::uint32 lastSaveCheck = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveSamplerProcessor)
};

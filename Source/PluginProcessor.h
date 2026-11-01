#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"
#include "SamplerPart.h"
#include "MasterEffects.h"
#include "Registers.h"

class LiveSamplerProcessor : public juce::AudioProcessor,
                             public juce::ChangeBroadcaster,
                             private juce::AsyncUpdater,
                             private juce::Timer
{
public:
    static constexpr int numParts = 3;
    static juce::String partName (int p)
    {
        static const char* names[] = { "Gornji 1", "Gornji 2", "Donji (split)" };
        return names[juce::jlimit (0, numParts - 1, p)];
    }
    static juce::String pid (int part, const char* name) { return "p" + juce::String (part) + "_" + name; }

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

    // --- za interfejs (message nit) ---
    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;

    void setLibraryFolder (const juce::File& folder);
    juce::File getLibraryFolder() const { return libraryFolder; }
    const juce::StringArray& getInstrumentList() const { return instrumentList; }

    void selectInstrument (int part, const juce::String& name);
    juce::String getInstrumentName (int part) const { return instrumentNames[part]; }

    bool getC3Is60() const { return c3Is60; }
    void setC3Is60 (bool shouldBe);

    Registers& getRegisters() { return registers; }
    void storeRegister (int index);
    void recallRegister (int index);
    int getCurrentRegister() const { return currentRegister; }

    juce::String getStatusText() const;
    void panic();

    // --- editor mapiranja ---
    InstrumentPtr getLoadedInstrument (const juce::String& name) const;
    void applyZones (const juce::String& name, std::vector<Zone> zones);   // odmah se cuje
    bool saveMap (const juce::String& name);
    void reloadInstrument (const juce::String& name, bool ignoreMapFile);   // ponisti izmene / vrati automatsko

private:
    void installInstrument (const juce::String& name, InstrumentPtr inst);
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleAsyncUpdate() override;
    void timerCallback() override;

    using LoadCallback = std::function<void (InstrumentPtr)>;
    void requestInstrument (const juce::String& name, LoadCallback cb, bool ignoreMapFile = false);
    void onInstrumentLoaded (const juce::String& name, InstrumentPtr inst, int epoch);
    void preloadRegisterInstruments();
    void rescanLibrary();

    void readSettings (PartSettings* ps, MasterSettings& ms);
    void processChunk (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi,
                       int chunkStart, int len, const PartSettings* ps, const MasterSettings& ms);
    void handleMidi (const juce::MidiMessage& m, const PartSettings* ps);

    // audio
    SamplerPart parts[numParts];
    MasterEffects master;
    juce::AudioBuffer<float> mainBuf, reverbBuf, delayBuf;
    int maxBlock = 512;
    juce::uint8 routed[16][128] {};
    std::atomic<int> pendingProgram { -1 };
    std::atomic<bool> panicRequested { false };

    // biblioteka i ucitavanje
    SampleLibrary library;
    juce::File libraryFolder;
    juce::StringArray instrumentList;
    juce::String instrumentNames[numParts];
    int partRequestId[numParts] {};
    bool c3Is60 = true;
    int loadEpoch = 0;
    std::map<juce::String, InstrumentPtr> cache;
    std::map<juce::String, std::vector<LoadCallback>> pendingLoads;
    std::vector<InstrumentPtr> graveyard;
    bool scanning = false;
    std::atomic<int> scanCounter { 0 };
    juce::String lastWarning;
    juce::ThreadPool loadPool { 1 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    // podesavanja i registri
    juce::ApplicationProperties appProps;
    Registers registers;
    int currentRegister = -1;
    juce::File getRegistersFile();

    // brzi pristup parametrima
    struct PartParamPtrs
    {
        std::atomic<float> *on, *vol, *pan, *oct, *trans, *fine, *velsens, *mono, *porta,
                           *att, *dec, *sus, *rel, *cutoff, *reso, *fxtype, *fxrate, *fxdepth,
                           *fxmix, *rev, *dly, *bend;
    } pp[numParts];
    std::atomic<float> *splitOn, *splitPoint, *lowerCh;
    std::atomic<float> *revSize, *revDamp, *revLevel, *dlyTime, *dlyFb, *dlyLevel,
                       *eqLow, *eqMid, *eqHigh, *compThr, *compRatio, *masterVol;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveSamplerProcessor)
};

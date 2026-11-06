#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"
#include "SamplerPart.h"
#include "MasterEffects.h"
#include "Model.h"
#include "KronosPcg.h"
#include "PluginHost.h"

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
    const juce::String getName() const override { return "Pikaso Workstation"; }
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
    juce::File getInstrumentFile (const juce::String& name) const { return library.instrumentFile (name); }   // folder / fajl na disku
    bool isScanning() const { return scanning; }
    bool getC3Is60() const { return c3Is60; }
    void setC3Is60 (bool shouldBe);

    // Useri
    int getNumUsers() const { return (int) users.size(); }
    const UserProgram& getUser (int i) const { return users[(size_t) juce::jlimit (0, juce::jmax (0, (int) users.size() - 1), i)]; }
    int findUser (const juce::String& name) const;
    juce::StringArray userUsage (const juce::String& name) const;   // performansi u kojima se user koristi ("1-003 Ime")
    juce::StringArray getUserNames() const;
    int createUser (const juce::String& baseName, const UserProgram* copyFrom = nullptr);
    int createUserFromInstrument (const juce::String& instrument);
    void updateUser (int index, const UserProgram& u);         // promena se cuje odmah
    void deleteUser (int index);
    // vrati usera na original: Kronos user se ponovo cita iz PCG-a, ostali iz snimka napravljenog pri pravljenju
    bool restoreUserOriginal (int index, juce::String& error);
    bool canRestoreUserOriginal (int index) const;
    void moveUser (int from, int to);                         // broj usera = mesto u listi
    const juce::StringArray& getUserCategories() const { return userCategories; }
    void editUserCategory (const juce::String& oldName, const juce::String& newName);   // old "" = new, new "" = delete
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
    juce::String getBankName (int b) const;
    void setBankName (int b, const juce::String& n);
    void clearPerformanceSlot (int slot);
    // scene u trenutnom performansu (8): pozovi = trenutno, sacuvaj = i u mesto u listi
    void recallScene (int s);
    void storeScene (int s);
    void clearScene (int s);
    void renameScene (int s, const juce::String& name);
    void swapPerformanceSlots (int a, int b);   // premestanje u listama (prazno mesto = samo premesti); set liste prate

    // kategorije performansa (iste kao kategorije uredjaja "Pikaso Workstation" u Live Kontroleru); performans pamti ime kategorije
    const juce::StringArray& getCategories() const { return categories; }
    // nova lista; remap[stari indeks] = novi indeks ili -1 (obrisana)
    void setCategories (const juce::StringArray& names, const std::vector<juce::Colour>& colours, const std::vector<int>& remap);
    void setSlotCategory (int slot, int category);          // -1 = bez kategorije
    int categoryIndex (const Performance& p) const { return p.category.isEmpty() ? -1 : categories.indexOf (p.category, true); }

    // veza sa Live Kontrolerom (samo uredjaj "Pikaso Workstation", Kronos i ostali se ne diraju):
    // cita se podesavanja-server.json, a izmene odavde se salju preko most.py (http://localhost:8080/state)
    bool isKontrolerLinked() const { return kontrolerLinked; }
    juce::File getKontrolerFile() const { return kontrolerFile; }
    void linkKontroler (const juce::File& f);              // File() = prekini vezu
    bool syncKontroler (juce::String* report = nullptr);
    static juce::File defaultKontrolerFile();
    juce::String categoryName (int c) const { return c >= 0 && c < categories.size() ? categories[c] : juce::String(); }
    juce::Colour categoryColour (int c) const { return c >= 0 && c < (int) categoryColours.size() ? categoryColours[(size_t) c] : juce::Colour (0xff8a93a6); }
    const std::vector<juce::Colour>& getCategoryColours() const { return categoryColours; }
    static juce::StringArray defaultCategories();             // kao Kronos kategorije u Live Kontroleru
    static std::vector<juce::Colour> defaultCategoryColours();
    void kontrolerChanged();                               // izmena ovde: posalji u Live Kontroler
    juce::String getKontrolerStatus() const;

    // set liste (velika dugmad za svirku)
    const SetList& getSetList (int i) const { return setLists[(size_t) juce::jlimit (0, kNumSetLists - 1, i)]; }
    void updateSetList (int i, const SetList& s);
    int getCurrentSetList() const { return currentSetList; }
    void setCurrentSetList (int i);
    int getSetListPage() const { return setListPage; }
    void setSetListPage (int p);
    int getCurrentSetListSlot() const { return currentSetListSlot; }
    void recallSetListSlot (int slot);          // u trenutnoj set listi
    // Program Change: 0 = performansi (CC0 bira listu), 1 = set lista (PC bira mesto u set listi)
    int getProgramChangeMode() const { return pcMode; }
    void setProgramChangeMode (int m) { pcMode = juce::jlimit (0, 1, m); markSave(); }

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
    void previewSample (SamplePtr s);      // presluskivanje sempla (MAPPING), svira jednom
    void stopPreview();
    InstrumentPtr getLoadedInstrument (const juce::String& name) const;
    void applyZones (const juce::String& name, std::vector<Zone> zones);
    void setRoundRobinRandom (const juce::String& name, bool random);
    bool saveMap (const juce::String& name);
    void reloadInstrument (const juce::String& name, bool ignoreMapFile);
    void ensureLoaded (const juce::String& name) { if (instrumentList.contains (name) && cache.count (name) == 0) requestInstrument (name, nullptr); }

    // Korg Kronos
    bool loadKronosPcg (const juce::File& f, juce::String& error);
    const Kronos::Pcg* getKronosPcg() const { return kronosPcg.get(); }
    juce::File getKronosFile() const { return kronosFile; }
    juce::String findKronosMultisample (const juce::String& uuid, int ms) const { return library.findKronosMultisample (uuid, ms); }
    // instrument za Kronos zonu: tvoj sempl iz KSC-a ili presemplovan ROM (prazno = nema)
    juce::String resolveKronosZone (const Kronos::Zone& z) const
    {
        if (z.user) return library.findKronosMultisample (z.uuid, z.ms);
        if (z.romKey.isNotEmpty()) return library.findKronosRom (z.romKey);
        return {};
    }
    // presemplovan EXi program (ceo program snimljen Kronos Autosamplerom)
    juce::String resolveKronosExi (const Kronos::Program& p) const { return p.exi ? library.findKronosRom (p.exiKey) : juce::String(); }
    // presemplovan drum kit program (svaki taster snimljen posebno)
    juce::String resolveKronosDrum (const Kronos::Program& p) const { return p.drumKit && p.drumKey.isNotEmpty() ? library.findKronosRom (p.drumKey) : juce::String(); }
    // MOD-7 program koji FM sinteza moze da svira (samo MOD-7, bez drugih EXi motora, i poznate veze)
    static bool kronosFmCapable (const Kronos::Program& p);
    static Fm::Patch kronosFmPatch (const Kronos::Program& p);
    bool preferFm = false;      // Kronos uvoz: MOD-7 programi kao FM sinteza i kad su presemplovani
    bool kronosProgramPlayable (const Kronos::Program& p) const
    {
        if (p.exi) return resolveKronosExi (p).isNotEmpty() || kronosFmCapable (p);
        if (p.drumKit) return resolveKronosDrum (p).isNotEmpty();
        if (! p.hd1 || p.drumKit) return false;
        for (int o = 0; o < (p.doubleMode ? 2 : 1); ++o)
            for (auto& z : p.osc[(size_t) o])
                if (z.used && resolveKronosZone (z).isNotEmpty()) return true;
        return false;
    }
    int createUserFromKronos (const Kronos::Program& p);   // -1 ako nema user semplova u biblioteci
    int createUserFromKronosExi (const Kronos::Program& p);
    int createUserFromKronosDrum (const Kronos::Program& p);
    // timbre kombija ciji zvuk jos nije presemplovan: part ostaje u performansu sa imenom programa
    struct KronosMissing { juce::uint32 bank = 0; int number = 0; juce::String label, where; bool canResample = false; };
    // posle osvezavanja biblioteke: partovi koji cekaju presemplovan Kronos program dobijaju usera sami
    int fillMissingParts();
    // import iz Kronosa: part koji ceka nepresemplovan zvuk se brise (part postaje prazan i iskljucen).
    // where = "Ime kombija T3, Drugi kombi T1" iz KronosMissing; vraca koliko partova je obrisano
    int removeMissingParts (const KronosMissing& m);
    // velocity slojevi presemplovanog zvuka (iz imena fajlova "060 v127.wav"): jacine od najjace
    std::vector<int> velocityLayersOf (const juce::String& instrument) const;
    // brise jedan velocity sloj sa diska; susedni sloj preuzima njegov opseg (i u mapi i u MS-ovima usera)
    bool deleteVelocityLayer (int userIndex, int oscIndex, juce::String& report);
    // jedan MS sa vise slojeva -> svaki sloj na svoj MS u istom OSC-u
    bool splitVelocityLayers (int userIndex, int oscIndex, juce::String& report);
    std::vector<float> measureLayerLevels (const juce::String& instrument, const std::vector<int>& layers) const;
    bool layoutVelocityLayers (UserProgram& user, int block, const OscSettings& proto, const std::vector<int>& layers);
    // novi prazan (inicijalizovan) performans na mestu u listi
    void initPerformanceSlot (int slot);

    // kriva jacine (velocity) za MIDI ulaz: da isti udarac na Impulse-u zvuci kao na Kronosu
    int getVelocityCurve() const { return velocityCurve.load(); }
    void setVelocityCurve (int c) { velocityCurve = juce::jlimit (0, 6, c); markSave(); }
    int shapeVelocity (int v) const
    {
        static const float gamma[] = { 1.0f, 0.85f, 0.7f, 0.58f, 0.48f, 1.25f, 1.5f };
        const float g = gamma[juce::jlimit (0, 6, velocityCurve.load())];
        if (v <= 0 || g == 1.0f) return v;
        return juce::jlimit (1, 127, (int) std::round (127.0f * std::pow ((float) v / 127.0f, g)));
    }

    // BACKUP: ceo projekat (useri, performansi, liste, set liste...) + svi semplovi koje useri koriste,
    // u jedan folder; na drugom racunaru "VRATI BACKUP" ga ucita
    bool startBackup (const juce::File& destFolder);
    bool isBackupRunning() const;
    juce::String getBackupStatus() const;
    bool restoreBackup (const juce::File& backupFolder, juce::String& report);
    bool isRestorePending() const { return restorePending; }

    // vector joystick: X / Y dolaze kao MIDI CC (iPad, Impulse...), -1..1; LEARN uzima prvi CC koji stigne
    float getVectorX() const { return vectorX.load(); }
    float getVectorY() const { return vectorY.load(); }
    void setVectorXY (float x, float y) { vectorX = juce::jlimit (-1.0f, 1.0f, x); vectorY = juce::jlimit (-1.0f, 1.0f, y); vectorUiDirty = true; }
    int getVectorCc (int axis) const { return axis == 0 ? vectorCcX.load() : vectorCcY.load(); }
    void learnVectorCc (int axis) { learnVector = axis + 1; }      // 0 = X, 1 = Y
    bool isLearningVector() const { return learnVector.load() != 0; }
    bool takeVectorUiDirty() { return vectorUiDirty.exchange (false); }
    int userForKronosProgram (const Kronos::Program& p);
    int createUserFromKronosFm (const Kronos::Program& p);   // FM sinteza (MOD-7) umesto semplova
    int createVlUser (int model);                             // VL fizicki model (Vl::Model)
    int createPluginUser (const juce::PluginDescription& d);  // VST3 instrument
    PluginHost& getPluginHost() { return pluginHost; }
    void savePluginState (int userIndex);                     // stanje plugina (posle editovanja) u usera
    std::shared_ptr<HostedPlugin> pluginFor (int userIndex);
    std::shared_ptr<const Vl::Patch> vlPatchFor (const UserProgram& u);
    std::map<juce::int64, std::shared_ptr<const Vl::Patch>> vlCache;
    std::shared_ptr<const Fm::Patch> fmPatchFor (const UserProgram& u);
    int createFmUser();                                        // novi FM user (jednostavan DX patch)   // postojeci user istog imena ili novi
    // Kombi -> trenutni performans (16 delova). Vraca spisak timbre-a koji nisu mogli da se prenesu.
    // keepChannels = false: svi delovi primaju sa svih MIDI kanala (jedna klavijatura)
    juce::StringArray loadKronosCombi (const Kronos::Combi& c, bool keepChannels, std::vector<KronosMissing>* missing = nullptr);

    // LIVE: transpose celog instrumenta, zakljucavanje editovanja na bini, sledeci / prethodni performans
    int getGlobalTranspose() const { return globalTranspose; }
    void setGlobalTranspose (int st) { globalTranspose = juce::jlimit (-24, 24, st); rebuildConfig(); markSave(); sendChangeMessage(); }
    bool isLiveLocked() const { return liveLock; }
    void setLiveLock (bool b) { liveLock = b; markSave(); sendChangeMessage(); }
    void stepPerformance (int dir);
    // orijentalno stimovanje performansa (cetvrttonovi)
    void setTuning (bool on);
    void setTuningPreset (int preset, int root);
    void setTuneCents (int pitchClass, float cents);
    // LIVE alati: opterecenje zvuka, polifonija, GIG rezim, split / opseg sviranjem tastera
    float getAudioLoad() const { return audioLoad.load(); }          // 0..1 (deo vremena bloka koji trosi zvuk), vrh sa padom
    int getDropouts() const { return dropouts.load(); }
    // VOICE GUARD: kad zvuk trosi > ~80% vremena bloka, gase se najstariji pusteni glasovi (pre krckanja).
    // Svaki dogadjaj (gasenje, limit polifonije, prekid zvuka) se upisuje u voice-guard-log.txt sa imenom performansa.
    bool getVoiceGuard() const { return voiceGuard.load(); }
    void setVoiceGuard (bool b) { voiceGuard = b; markSave(); }
    static juce::File getGuardLogFile();              // blokovi koji su zakasnili (pucketanje)
    void resetDropouts() { dropouts = 0; }
    // zvucna kartica (samostalni program): drajver, bafer, ukupna latencija; warn = nije ASIO / velika latencija / pucketanje
    struct AudioInfo { juce::String driver, device; int buffer = 0; double rate = 0, latencyMs = 0; bool warn = false; juce::String advice; };
    AudioInfo getAudioInfo() const;
    juce::String getAudioShortText() const;     // "CPU 12% | 5.8 ms ASIO"
    // SOLO: samo oznaceni partovi sviraju (privremeno, ne snima se; brise se kad se pozove drugi performans)
    void setSolo (int part, bool on);
    // LOCK PART: part (npr. bas) ostaje isti kad se menjaju performansi, a u njegovoj zoni tastera svira samo on
    void setPartLock (int part, bool on);
    bool isPartLocked (int part) const { return part >= 0 && part < kNumParts && partLocked[(size_t) part]; }
    bool isSolo (int part) const { return part >= 0 && part < 16 && ((soloMask >> part) & 1) != 0; }
    bool anySolo() const { return soloMask != 0; }
    void clearSolo();
    // USER presluskivanje: user svira sam (bez performansa); -1 = svira performans
    void setAuditionUser (int userIndex);
    int getAuditionUser() const { return auditionUser; }
    bool getShowKeyRows() const { return showKeyRows; }        // PERFORMANCE: redovi Low/High/Vel/Channel na partovima
    void setShowKeyRows (bool b) { showKeyRows = b; markSave(); sendChangeMessage(); }
    bool getShowActiveOnly() const { return showActiveOnly; }  // PERFORMANCE: prikazi samo ukljucene partove
    void setShowActiveOnly (bool b) { showActiveOnly = b; markSave(); sendChangeMessage(); }
    bool isDiskStreaming() const { return SampleLibrary::streamingEnabled.load(); }
    void setDiskStreaming (bool on);       // ponovo ucita sve semplove u RAM-u
    int getMaxPolyphony() const { return maxPolyphony; }
    void setMaxPolyphony (int n);
    bool isGigMode() const { return gigMode; }
    void setGigMode (bool on);
    void applyGigMode();
    // mode 1: sledeca dva tastera = najnizi i najvisi; mode 2: sledeci taster = split (ovaj part od tog tastera navise)
    void armKeyLearn (int part, int mode) { keyLearnFirst = -1; keyLearnPart = part; keyLearnMode = mode; sendChangeMessage(); }
    void cancelKeyLearn() { keyLearnMode = 0; sendChangeMessage(); }
    int getKeyLearnMode() const { return keyLearnMode.load(); }
    int getKeyLearnPart() const { return keyLearnPart.load(); }
    // IMPULSE (iPad): pregled i upravljanje Novation Impulse-om. Workstation ga cita direktno (MIDI ulaz),
    // iPad pomera kontrole kao da su pomerene na Impulse-u; pravi fader se preuzima tek kad prodje kroz vrednost (pickup).
    struct ImpulseCtl { juce::String id, kind, name; int type = 1; int num = 0; int ch = 0; };   // type: 1 CC, 2 nota, 3 pitch
    const std::vector<ImpulseCtl>& getImpulseLayout() const { return impCtls; }
    int getImpulseValue (const ImpulseCtl&) const;
    int getImpulseChannel() const { return impCh.load(); }
    bool isImpulseSeen() const { return impSeen.load(); }
    void impulseFromRemote (const juce::String& id, int value);
    juce::String impulseTargetText (const ImpulseCtl&, int& state) const;   // state: -1 nema, 0 iskljuceno, 1 ukljuceno
    void loadImpulseLayout (const juce::var& kontrolerJson);
    void impulseIncoming (const juce::MidiMessage&);   // MIDI nit: samo pamti vrednosti
    bool isQtEdit() const { return qtEdit.load(); }
    void setQtEdit (bool b) { qtEdit = b; sendChangeMessage(); }
    // moji preseti reverba (cuvaju se u podesavanjima Workstation-a)
    const std::vector<ReverbPreset>& getUserReverbPresets() const { return userReverbPresets; }
    void saveUserReverbPreset (const juce::String& name, const MasterSettings& m)
    {
        for (auto& x : userReverbPresets) if (x.name.equalsIgnoreCase (name)) { x = ReverbPreset::from (name, m); markSave(); sendChangeMessage(); return; }
        userReverbPresets.push_back (ReverbPreset::from (name, m)); markSave(); sendChangeMessage();
    }
    void deleteUserReverbPreset (int i) { if (i >= 0 && i < (int) userReverbPresets.size()) { userReverbPresets.erase (userReverbPresets.begin() + i); markSave(); sendChangeMessage(); } }
    const GlobalEqSettings& getGlobalEq() const { return globalEq; }
    void setGlobalEq (const GlobalEqSettings& e) { globalEq = e; rebuildConfig(); markSave(); sendChangeMessage(); }
    float getGlobalVolume() const { return globalDb; }
    void setGlobalVolume (float db) { globalDb = db; rebuildConfig(); markSave(); }

    juce::String getStatusText() const;
    int getActiveVoices() const { return activeVoices.load(); }
    // aktivnost partova (tackice) i jacina pritisnutih dirki (virtuelna klavijatura)
    // VU metri: vrh izlaza parta / mastera od poslednjeg citanja (UI cita ~30 puta u sekundi)
    float takePartPeak (int p, int ch) { return p >= 0 && p < kNumParts ? parts[p].takePeak (ch) : 0.0f; }
    float takeMasterPeak (int ch) { return masterPeak[ch & 1].exchange (0.0f); }
    float getMasterRms (int ch) const { return masterRms[ch & 1].load(); }
    // AUTO LEVEL: svaki user se odsvira sam na najjacoj dinamici, izmeri se izlaz i podesi COMMON Level
    // da svi budu iste jacine (zakljucani se ne diraju). userIndex = -1: svi
    void startAutoLevel (float targetDb, int userIndex = -1);
    bool isAutoLevelRunning() const { return autoLevelRunning.load(); }
    juce::String getAutoLevelStatus() const { const juce::ScopedLock sl (autoLevelLock); return autoLevelStatus; }
    float getAutoLevelTarget() const { return autoLevelTarget; }
    void setAutoLevelTarget (float db) { autoLevelTarget = juce::jlimit (-30.0f, -6.0f, db); markSave(); }
    int getPartVoices (int p) const { return p >= 0 && p < 16 ? partVoices[(size_t) p].load() : 0; }
    juce::uint32 getPartHits (int p) const { return p >= 0 && p < 16 ? partHits[(size_t) p].load() : 0; }
    int getKeyVelocity (int note) const { return note >= 0 && note < 128 ? keyVel[(size_t) note].load() : 0; }
    void panic();

    juce::File getDataFile() const;
    void saveNow();

private:
    PluginHost pluginHost;        // prvi clan: instance pluginova se gase poslednje (posle svih config-a)
    int midiPos = 0;              // audio nit: polozaj trenutnog MIDI dogadjaja u bloku (za plugin)
    struct EngineConfig
    {
        std::array<PartRuntime, kNumParts> parts;
        juce::uint16 lockedMask = 0;   // LOCK PART: u zoni zakljucanog parta svira samo on
        bool anyPlugin = false;        // neki part svira VST plugin
        MasterSettings master;
        GlobalEqSettings globalEq;
        float globalDb = 0.0f;
        int globalTranspose = 0;
        int maxVoices = 128;
        juce::uint32 perfSerial = 0;    // menja se kad se pozove drugi performans (SST)
        TercaSettings terca;
        int harmNative = -1;            // skala za koju je napravljena ugradjena terca performansa (-1 = nema)
    };
    void processPlugins (const EngineConfig& c, int len);
    using ConfigPtr = std::shared_ptr<const EngineConfig>;

    void handleAsyncUpdate() override;
    void timerCallback() override;

    void rebuildConfig();
    void applyKronosScale (const Kronos::Combi&, Performance&) const;
    void rememberHarmKey();
    static int fixedHarmInterval (const PerfPart&, int note, int root, int scale, const EngineConfig&);
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
    // dva kompleta delova: "parts" svira, "tails" pusta da izdisu zvuci prethodnog performansa (Smooth Sound Transition)
    SamplerPart partBanks[2][kNumParts];
    struct PartSpan
    {
        SamplerPart* d;
        SamplerPart* begin() const { return d; }
        SamplerPart* end() const { return d + kNumParts; }
        SamplerPart& operator[] (int i) const { return d[i]; }
    };
    PartSpan parts { partBanks[0] }, tails { partBanks[1] };
    ConfigPtr tailConfig;               // konfiguracija prethodnog performansa dok mu zvuci izdisu
    juce::uint32 perfSerial = 0;
    juce::uint32 soloMask = 0;
    int auditionUser = -1;
    bool sustainHeld = false;           // audio nit
    MasterEffects master;
    juce::AudioBuffer<float> mainBuf, reverbBuf, delayBuf;
    int maxBlock = 512;
    juce::uint16 routed[16][128] {};       // bit = deo (16 delova)
    juce::uint16 harmonized[16][128] {};
    // terca za MONO boje: prati notu koja stvarno svira (prioritet usera) i prelazi legato zajedno sa melodijom
    struct MonoHarm
    {
        int note[32] {}, vel[32] {}, n = 0;      // drzani tasteri ovog dela, redom pritiska
        int curId = -1, curMel = -1, root = -1, scale = -1;
        bool on = false;
        int tailId = -1, tailNote[32] {}, tailN = 0;   // terca koja je ostala u prethodnom performansu (SST)
        void add (int nt, int v) { remove (nt); if (n < 32) { note[n] = nt; vel[n] = v; ++n; } }
        void remove (int nt) { for (int i = 0; i < n; ++i) if (note[i] == nt) { for (int k = i; k + 1 < n; ++k) { note[k] = note[k + 1]; vel[k] = vel[k + 1]; } --n; return; } }
        void reset() { n = 0; curId = curMel = root = scale = -1; on = false; }
    };
    std::array<MonoHarm, kNumParts> monoHarm;
    void monoHarmUpdate (int part, const EngineConfig&);
    int heldCount[128] {};
    bool tercaCapturing = false, recognizedInCapture = false;
    int capturedMask = 0;

    juce::SpinLock configLock;
    ConfigPtr pendingConfig;      // poslednja verzija (message nit)
    ConfigPtr audioConfig;        // koju audio nit koristi
    std::vector<ConfigPtr> configGraveyard;

    std::atomic<int> pendingProgram { -1 }, pendingBankMsb { -1 };
    std::atomic<bool> panicRequested { false };
    std::atomic<bool> tercaOn { false };
    std::atomic<int> tercaRoot { 2 }, tercaScale { 1 };
    std::atomic<bool> learning { false };
    std::atomic<int> learnedType { 0 }, learnedChannel { 0 }, learnedNumber { 0 };
    std::atomic<bool> tercaChanged { false };
    std::atomic<int> activeVoices { 0 };
    std::atomic<float> masterPeak[2] { 0.0f, 0.0f }, masterRms[2] { 0.0f, 0.0f };
    std::atomic<bool> autoLevelRunning { false };
    juce::CriticalSection autoLevelLock;
    juce::String autoLevelStatus;
    float autoLevelTarget = -18.0f;
    std::unique_ptr<juce::Thread> autoLevelThread;
    std::array<std::atomic<int>, 16> partVoices {};
    std::array<std::atomic<juce::uint32>, 16> partHits {};
    std::array<std::atomic<int>, 128> keyVel {};
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
    juce::SpinLock previewLock;
    SamplePtr previewSmp;
    double previewPos = 0.0;
    SamplePtr previewOld;                  // oslobadja se na message niti
    std::map<juce::int64, std::shared_ptr<const Fm::Patch>> fmCache;
    std::map<juce::String, std::vector<LoadCallback>> pendingLoads;
    std::vector<InstrumentPtr> graveyard;
    juce::String lastWarning;
    juce::ThreadPool loadPool { 1 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    std::unique_ptr<Kronos::Pcg> kronosPcg;
    juce::File kronosFile;

    // korisnicki podaci
    std::vector<UserProgram> users;
    juce::StringArray userCategories { "Piano", "E.Piano", "Organ", "Keys", "Strings", "Brass", "Woodwind", "Guitar", "Bass",
                                       "Synth Lead", "Synth Pad", "Choir", "Drums", "Percussion", "Ethnic", "FX" };
    std::array<Performance, kNumPerfSlots> slots;
    Performance perf;
    int currentSlot = -1, currentBank = 0;
    std::array<juce::String, kNumPerfBanks> bankNames;
    std::atomic<float> vectorX { 0.0f }, vectorY { 0.0f };
    std::atomic<int> vectorCcX { 16 }, vectorCcY { 17 }, learnVector { 0 };
    std::atomic<bool> vectorUiDirty { false };
    std::atomic<int> velocityCurve { 0 };
    juce::StringArray categories { defaultCategories() };
    std::vector<juce::Colour> categoryColours { defaultCategoryColours() };
    bool categoriesEdited = false;
    std::map<int, juce::String> kontrolerLast;               // mesto -> kategorija kako je bila u Live Kontroleru pri poslednjem citanju
    std::map<juce::String, juce::String> kontrolerRenames;   // preimenovane kategorije (staro -> novo) za slanje
    friend class KontrolerPusher;
    std::unique_ptr<juce::Thread> kontrolerPusher;
    juce::String kontrolerStatus;
    friend class BackupThread;
    std::unique_ptr<juce::Thread> backupThread;
    std::unique_ptr<class RemoteServer> remote;
    std::vector<ImpulseCtl> impCtls;
    std::array<std::atomic<int>, 128> impCc {}, impNote {};
    std::atomic<int> impPb { 64 }, impCh { 0 };
    std::atomic<bool> impSeen { false };
    std::array<std::atomic<int>, 128> impPickup {};
    std::array<std::atomic<bool>, 128> impLatch {};          // CC dugmadi / transporta na Impulse-u (toggle mod)
    std::array<std::atomic<bool>, 128> impMomentary {};      // primeceno: dugme se pusta odmah (momentary)
    std::array<juce::uint32, 128> impDownMs {};               // MIDI nit
public:
    // toggle mod; dugme prebaceno na Impulse-u u momentary (brzo pusta: 127 pa 0 za < 350 ms) radi klasicno: pritisak / drzanje
    bool isImpulseLatching (int channel, int cc) const
    { return cc >= 0 && cc < 128 && channel == impCh.load() && impLatch[(size_t) cc].load() && ! impMomentary[(size_t) cc].load(); }
private:          // -1 = nema; inace vrednost sa iPada koju pravi kontroler mora da stigne
    std::array<int, 128> impPhysPrev {};                     // audio nit
    juce::AbstractFifo impFifo { 256 };
    std::array<juce::uint32, 256> impFifoData {};
    std::unique_ptr<juce::MidiInputCallback> impCallback;
    juce::StringArray impRegistered;
    void registerImpulseInputs();
    bool impInjecting = false;   // audio nit     // iPad (Live Kontroler, tab PIKASO), port 8091
    bool restorePending = false;   // backup vracen: ne snimaj preko njega dok se program ne pokrene ponovo
    bool kontrolerLinked = false, kontrolerChecked = false;
    juce::File kontrolerFile;
    juce::Time kontrolerTime;
    int kontrolerTick = 0;
    // MIDI ulazi (samostalni program): imena koja su bila ukljucena se ponovo ukljuce kad se port pojavi
    // (loopMIDI / USB uredjaj koji se upali posle programa)
    juce::StringArray wantedMidiInputs;
    juce::StringArray lastMidiPresent, lastMidiEnabled;
    int midiTick = 0;
    void keepMidiInputs();
    std::array<SetList, kNumSetLists> setLists;
    int currentSetList = 0, setListPage = 0, currentSetListSlot = -1, pcMode = 0;
    bool perfDirty = false;
    TercaSettings terca;
    int globalTranspose = 0;
    bool liveLock = false;
    std::atomic<bool> qtEdit { false };
    std::atomic<int> qtMask { 0 };
    std::atomic<float> audioLoad { 0.0f };
    std::atomic<int> dropouts { 0 };
    std::atomic<bool> voiceGuard { true };
    std::atomic<float> lastBlockLoad { 0.0f };
    std::atomic<int> guardShed { 0 }, guardDropouts { 0 };
    std::atomic<float> guardPeakLoad { 0.0f };
    // zapis dogadjaja (message nit, iz timerCallback): jedan red na najvise 5 s po performansu
    struct GuardEpisode { int shed = 0, stolen = 0, drops = 0, maxVoices = 0; float peak = 0.0f; juce::String perfLabel; juce::uint32 lastWrite = 0; bool any = false; } guardEp;
    void guardTick();
    int maxPolyphony = 128;
    bool showKeyRows = false;
    bool showActiveOnly = false;
    std::array<bool, kNumParts> partLocked {};
    std::array<PerfPart, kNumParts> lockedParts, ownParts;   // zakljucan part / part koji performans sam ima na tom mestu
    void applyPartLocks (Performance& p);                    // ubaci zakljucane partove (ownParts pamti sopstvene)
    bool gigMode = false;
    std::atomic<int> keyLearnMode { 0 }, keyLearnPart { 0 }, keyLearnFirst { -1 }, keyLearnNote { -1 };
    GlobalEqSettings globalEq;
    std::vector<ReverbPreset> userReverbPresets;
    float globalDb = 0.0f;
    bool saveNeeded = false;
    juce::uint32 lastSaveCheck = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveSamplerProcessor)
};

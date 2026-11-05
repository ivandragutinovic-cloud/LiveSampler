#include "PluginProcessor.h"
#include "MidiLearn.h"
#include "RemoteServer.h"
#include "PluginEditor.h"
#include "Text.h"
#include "AutoLoop.h"
#include "DiskStreamer.h"
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

juce::Colour LiveSamplerProcessor::partColour (int i)
{
    static const juce::uint32 c[] = { 0xffff8a3d, 0xff3dc5ff, 0xff7ddf64, 0xffff5c8a,
                                      0xffc58aff, 0xffffd23d, 0xff3dffd0, 0xffff9be0 };
    return juce::Colour (c[(size_t) ((i % 8) + 8) % 8]);
}

//==============================================================================
// pad programa: zapis u %APPDATA%\LiveSampler\crash-log.txt (gde je puklo), da moze da se nadje uzrok
static void pikasoCrashHandler (void*)
{
    const auto f = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("LiveSampler").getChildFile ("crash-log.txt");
    f.appendText ("\n==== " + juce::Time::getCurrentTime().toString (true, true) + "  " + juce::String (ProjectInfo::versionString) + "\n"
                  + juce::SystemStats::getStackBacktrace() + "\n");
}

LiveSamplerProcessor::LiveSamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    juce::SystemStats::setApplicationCrashHandler (pikasoCrashHandler);
    MidiLearn::get();     // napravi na glavnoj niti (audio nit ga samo cita)
    terca = TercaSettings::makeDefault();
    loadData();
    rebuildConfig();
    if (! libraryFolders.isEmpty())
        rescanLibrary();
    for (auto& x : impPickup) x = -1;
    impPhysPrev.fill (-1);
    loadImpulseLayout ({});
    MidiLearn::get().isLatching = [this, a = alive] (int ch, int cc) { return a->load() && isImpulseLatching (ch, cc); };
    startTimer (500);
}

LiveSamplerProcessor::~LiveSamplerProcessor()
{
    alive->store (false);
    MidiLearn::get().isLatching = nullptr;
    stopTimer();
    remote.reset();
   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        for (auto& id : impRegistered) holder->deviceManager.removeMidiInputDeviceCallback (id, impCallback.get());
   #endif
    if (saveNeeded)
        saveNow();
    loadPool.removeAllJobs (true, 10000);
    if (kontrolerPusher != nullptr) kontrolerPusher->stopThread (3000);
    if (backupThread != nullptr) backupThread->stopThread (10000);
}

bool LiveSamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

//==============================================================================
//  Cuvanje podataka
//==============================================================================
juce::File LiveSamplerProcessor::getDataFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("LiveSampler").getChildFile ("LiveSampler-podaci.xml");
}

void LiveSamplerProcessor::saveNow()
{
    if (restorePending) return;
    juce::XmlElement root ("LiveSamplerData");
    root.setAttribute ("version", 3);
    root.setAttribute ("c3Is60", c3Is60);
    root.setAttribute ("globalDb", (double) globalDb);
    {
        auto* rp = root.createNewChildElement ("ReverbPresets");
        for (auto& x : userReverbPresets) rp->addChildElement (x.toXml().release());
        auto* e = root.createNewChildElement ("MasterEq");
        for (auto& k : GlobalEqSettings::names()) e->setAttribute (k, (double) globalEq.getByName (k));
    }
    root.setAttribute ("globalTranspose", globalTranspose);
    root.setAttribute ("liveLock", liveLock);
    root.setAttribute ("maxPolyphony", maxPolyphony);
    root.setAttribute ("gigMode", gigMode);
    root.setAttribute ("showKeyRows", showKeyRows);
    root.setAttribute ("midiInputs", wantedMidiInputs.joinIntoString ("|"));
    root.setAttribute ("diskStreaming", SampleLibrary::streamingEnabled.load());
    root.setAttribute ("currentSlot", currentSlot);
    root.setAttribute ("currentBank", currentBank);
    root.setAttribute ("kronosPcg", kronosFile.getFullPathName());

    auto* folders = root.createNewChildElement ("Folders");
    for (auto& f : libraryFolders)
        folders->createNewChildElement ("Folder")->setAttribute ("path", f.getFullPathName());

    auto* us = root.createNewChildElement ("Users");
    for (auto& u : users)
        us->addChildElement (u.toXml().release());

    auto* ps = root.createNewChildElement ("Slots");
    for (int i = 0; i < kNumPerfSlots; ++i)
    {
        if (! slots[(size_t) i].used) continue;
        auto x = slots[(size_t) i].toXml();
        x->setAttribute ("slot", i);
        ps->addChildElement (x.release());
    }

    root.setAttribute ("pcMode", pcMode);
    root.setAttribute ("vectorCcX", vectorCcX.load());
    root.setAttribute ("velocityCurve", velocityCurve.load());
    root.setAttribute ("vectorCcY", vectorCcY.load());
    root.setAttribute ("currentSetList", currentSetList);
    root.setAttribute ("setListPage", setListPage);
    auto* cats = root.createNewChildElement ("Categories");
    cats->setAttribute ("edited", categoriesEdited);
    cats->setAttribute ("version", 3);
    cats->setAttribute ("kontrolerLinked", kontrolerLinked);
    cats->setAttribute ("kontrolerFile", kontrolerFile.getFullPathName());
    for (int c = 0; c < categories.size(); ++c)
    {
        auto* x = cats->createNewChildElement ("Category");
        x->setAttribute ("name", categories[c]);
        x->setAttribute ("colour", categoryColour (c).toString());
    }

    auto* uc = root.createNewChildElement ("UserCategories");
    for (auto& c : userCategories) uc->createNewChildElement ("Category")->setAttribute ("name", c);

    auto* bn = root.createNewChildElement ("BankNames");
    for (int b = 0; b < kNumPerfBanks; ++b)
        if (bankNames[(size_t) b].isNotEmpty())
        {
            auto* x = bn->createNewChildElement ("Bank");
            x->setAttribute ("index", b);
            x->setAttribute ("name", bankNames[(size_t) b]);
        }
    auto* sls = root.createNewChildElement ("SetLists");
    for (int l = 0; l < kNumSetLists; ++l)
    {
        const auto& sl = setLists[(size_t) l];
        auto* x = sls->createNewChildElement ("SetList");
        x->setAttribute ("index", l);
        x->setAttribute ("name", sl.name);
        for (int i = 0; i < kSetListSlots; ++i)
        {
            const auto& s = sl.slots[(size_t) i];
            if (s.perf < 0) continue;
            auto* y = x->createNewChildElement ("Slot");
            y->setAttribute ("index", i);
            y->setAttribute ("perf", s.perf);
            y->setAttribute ("label", s.label);
            y->setAttribute ("colour", s.colour);
        }
    }

    auto cur = perf.toXml();
    cur->setTagName ("Current");
    root.addChildElement (cur.release());
    root.addChildElement (terca.toXml().release());

    auto file = getDataFile();
    file.getParentDirectory().createDirectory();
    root.writeTo (file);
    saveNeeded = false;
}

void LiveSamplerProcessor::loadData()
{
    auto xml = juce::XmlDocument::parse (getDataFile());
    if (xml == nullptr || ! xml->hasTagName ("LiveSamplerData"))
    {
        // prethodna verzija je pamtila jedan folder u LiveSampler.settings
        auto old = juce::XmlDocument::parse (getDataFile().getSiblingFile ("LiveSampler.settings"));
        if (old != nullptr)
            for (auto* v : old->getChildIterator())
            {
                if (v->getStringAttribute ("name") == "libraryFolder")
                {
                    juce::File f (v->getStringAttribute ("val"));
                    if (f.isDirectory()) libraryFolders.add (f);
                }
                if (v->getStringAttribute ("name") == "c3Is60")
                    c3Is60 = v->getStringAttribute ("val") != "0";
            }
        return;
    }

    c3Is60 = xml->getBoolAttribute ("c3Is60", true);
    globalDb = (float) xml->getDoubleAttribute ("globalDb", 0.0);
    userReverbPresets.clear();
    if (auto* rp = xml->getChildByName ("ReverbPresets"))
        for (auto* x : rp->getChildWithTagNameIterator ("ReverbPreset")) userReverbPresets.push_back (ReverbPreset::fromXml (*x));
    if (auto* e = xml->getChildByName ("MasterEq"))
    {
        for (auto& k : GlobalEqSettings::names())
            if (e->hasAttribute (k)) globalEq.setByName (k, (float) e->getDoubleAttribute (k));
        if (e->hasAttribute ("midDb")) { globalEq.m1Db = (float) e->getDoubleAttribute ("midDb"); globalEq.m1Hz = (float) e->getDoubleAttribute ("midHz", 1000.0); }
    }
    globalTranspose = juce::jlimit (-24, 24, xml->getIntAttribute ("globalTranspose", 0));
    liveLock = xml->getBoolAttribute ("liveLock", false);
    maxPolyphony = juce::jlimit (8, 128, xml->getIntAttribute ("maxPolyphony", 128));
    gigMode = xml->getBoolAttribute ("gigMode", false);
    showKeyRows = xml->getBoolAttribute ("showKeyRows", false);
    wantedMidiInputs = juce::StringArray::fromTokens (xml->getStringAttribute ("midiInputs"), "|", "");
    wantedMidiInputs.removeEmptyStrings();
    if (! xml->hasAttribute ("midiInputs")) wantedMidiInputs.add ("Pikaso-miniPC");   // dugmad kategorija iz Pikaso Stage
    SampleLibrary::streamingEnabled = xml->getBoolAttribute ("diskStreaming", false);
    if (SampleLibrary::streamingEnabled.load()) DiskStreamer::get();   // nit i baferi se prave ovde, ne na audio niti
    currentSlot = xml->getIntAttribute ("currentSlot", -1);
    currentBank = juce::jlimit (0, kNumPerfBanks - 1, xml->getIntAttribute ("currentBank", 0));
    kronosFile = juce::File (xml->getStringAttribute ("kronosPcg"));

    if (auto* folders = xml->getChildByName ("Folders"))
        for (auto* f : folders->getChildWithTagNameIterator ("Folder"))
            libraryFolders.add (juce::File (f->getStringAttribute ("path")));

    if (auto* us = xml->getChildByName ("Users"))
        for (auto* u : us->getChildWithTagNameIterator ("User"))
            users.push_back (UserProgram::fromXml (*u));

    const bool oldSlots = xml->getIntAttribute ("version", 2) < 3;   // ranije: 4 banke po 16
    if (oldSlots && currentSlot >= 0)
        currentSlot = (currentSlot / 16) * kPerfPerBank + currentSlot % 16;
    if (oldSlots)
        currentBank = 0;
    if (auto* ps = xml->getChildByName ("Slots"))
        for (auto* p : ps->getChildWithTagNameIterator ("Performance"))
        {
            int i = p->getIntAttribute ("slot", -1);
            if (oldSlots && i >= 0) i = (i / 16) * kPerfPerBank + i % 16;
            if (i >= 0 && i < kNumPerfSlots)
                slots[(size_t) i] = Performance::fromXml (*p);
        }

    pcMode = juce::jlimit (0, 1, xml->getIntAttribute ("pcMode", 0));
    vectorCcX = juce::jlimit (0, 127, xml->getIntAttribute ("vectorCcX", 16));
    velocityCurve = juce::jlimit (0, 6, xml->getIntAttribute ("velocityCurve", 0));
    vectorCcY = juce::jlimit (0, 127, xml->getIntAttribute ("vectorCcY", 17));
    currentSetList = juce::jlimit (0, kNumSetLists - 1, xml->getIntAttribute ("currentSetList", 0));
    setListPage = juce::jlimit (0, kSetListSlots / 16 - 1, xml->getIntAttribute ("setListPage", 0));
    if (auto* cats = xml->getChildByName ("Categories"))
    {
        categoriesEdited = cats->getBoolAttribute ("edited", false);
        kontrolerChecked = cats->hasAttribute ("kontrolerLinked");
        kontrolerLinked = cats->getBoolAttribute ("kontrolerLinked", false);
        kontrolerFile = juce::File (cats->getStringAttribute ("kontrolerFile"));
        if (cats->getIntAttribute ("version", 1) >= 3 || categoriesEdited)
        {
            categories.clear();
            categoryColours.clear();
            for (auto* x : cats->getChildWithTagNameIterator ("Category"))
            {
                categories.add (x->getStringAttribute ("name"));
                categoryColours.push_back (juce::Colour::fromString (x->getStringAttribute ("colour", "ff8a93a6")));
            }

        }
        else
        {
            // starije verzije (Korg imena / indeksi): prelazi se na kategorije iz Live Kontrolera
            for (auto& s : slots) s.category = {};
        }
        if (categories.isEmpty()) { categories = defaultCategories(); categoryColours = defaultCategoryColours(); }
        categoryColours.resize ((size_t) categories.size(), juce::Colour (0xff8a93a6));
    }
    if (auto* uc = xml->getChildByName ("UserCategories"))
    {
        userCategories.clear();
        for (auto* x : uc->getChildWithTagNameIterator ("Category"))
            if (x->getStringAttribute ("name").isNotEmpty()) userCategories.add (x->getStringAttribute ("name"));
    }
    if (auto* bn = xml->getChildByName ("BankNames"))
        for (auto* x : bn->getChildWithTagNameIterator ("Bank"))
            if (const int b = x->getIntAttribute ("index", -1); b >= 0 && b < kNumPerfBanks)
                bankNames[(size_t) b] = x->getStringAttribute ("name");
    if (auto* sls = xml->getChildByName ("SetLists"))
        for (auto* x : sls->getChildWithTagNameIterator ("SetList"))
        {
            const int l = x->getIntAttribute ("index", -1);
            if (l < 0 || l >= kNumSetLists) continue;
            auto& sl = setLists[(size_t) l];
            sl.name = x->getStringAttribute ("name");
            for (auto* y : x->getChildWithTagNameIterator ("Slot"))
            {
                const int i = y->getIntAttribute ("index", -1);
                if (i < 0 || i >= kSetListSlots) continue;
                sl.slots[(size_t) i].perf = y->getIntAttribute ("perf", -1);
                sl.slots[(size_t) i].label = y->getStringAttribute ("label");
                sl.slots[(size_t) i].colour = y->getIntAttribute ("colour", 0);
            }
        }

    if (auto* cur = xml->getChildByName ("Current"))
        perf = Performance::fromXml (*cur);
    if (auto* t = xml->getChildByName ("Terce"))
        terca = TercaSettings::fromXml (*t);
}

void LiveSamplerProcessor::bootstrapIfEmpty()
{
    // prvi put: napravi usera od prvog instrumenta i performans sa njim
    if (users.empty() && ! instrumentList.isEmpty())
    {
        const int u = createUserFromInstrument (instrumentList[0]);
        if (u >= 0 && ! perf.parts[0].on)
        {
            perf = Performance::makeDefault (users[(size_t) u].name);
            perfDirty = true;
        }
    }
}

//==============================================================================
//  Biblioteka
//==============================================================================
void LiveSamplerProcessor::addLibraryFolder (const juce::File& folder)
{
    if (! folder.isDirectory() || libraryFolders.contains (folder))
        return;
    libraryFolders.add (folder);
    markSave();
    rescanLibrary();
}

void LiveSamplerProcessor::removeLibraryFolder (int index)
{
    if (index < 0 || index >= libraryFolders.size())
        return;
    libraryFolders.remove (index);
    markSave();
    rescanLibrary();
}

void LiveSamplerProcessor::rescanLibrary()
{
    scanning = true;
    sendChangeMessage();

    auto folders = libraryFolders;
    auto aliveFlag = alive;
    const int scanId = ++scanCounter;
    loadPool.addJob ([this, folders, aliveFlag, scanId]
    {
        if (scanId != scanCounter.load())
            return;
        library.scan (folders);
        auto names = library.getInstrumentNames();

        juce::MessageManager::callAsync ([this, names, aliveFlag, scanId]
        {
            if (! aliveFlag->load() || scanId != scanCounter.load())
                return;

            scanning = false;
            instrumentList = names;
            fillMissingParts();

            // izbaci iz memorije instrumente kojih vise nema (npr. izvadjen USB)
            for (auto it = cache.begin(); it != cache.end();)
            {
                if (! names.contains (it->first)) { graveyard.push_back (it->second); it = cache.erase (it); }
                else ++it;
            }

            bootstrapIfEmpty();
            rebuildConfig();
            loadNeededInstruments();
            sendChangeMessage();
        });
    });
}

void LiveSamplerProcessor::setC3Is60 (bool shouldBe)
{
    if (c3Is60 == shouldBe)
        return;

    c3Is60 = shouldBe;
    markSave();

    ++loadEpoch;
    for (auto& kv : cache)
        graveyard.push_back (kv.second);
    cache.clear();
    pendingLoads.clear();
    rebuildConfig();
    loadNeededInstruments();
}

//==============================================================================
//  Ucitavanje instrumenata
//==============================================================================
juce::StringArray LiveSamplerProcessor::neededInstruments (bool includeBank) const
{
    juce::StringArray names;
    auto addPerf = [this, &names] (const Performance& p)
    {
        // i delovi koje neka scena pali (prelaz na scenu mora biti trenutan)
        for (size_t i = 0; i < p.parts.size(); ++i)
        {
            const auto& part = p.parts[i];
            bool needed = part.on;
            for (auto& sc : p.scenes) needed = needed || (sc.used && sc.parts[i].on);
            if (needed)
                if (const int u = findUser (part.user); u >= 0)
                    for (auto& n : users[(size_t) u].usedInstruments())
                        names.addIfNotAlreadyThere (n);
        }
    };

    addPerf (perf);
    if (includeBank)
    {
        // unapred: dugmad trenutne strane set liste (da prebacivanje na svirci bude trenutno)
        const auto& sl = setLists[(size_t) currentSetList];
        for (int i = setListPage * 16; i < setListPage * 16 + 16; ++i)
            if (const int p = sl.slots[(size_t) i].perf; p >= 0 && p < kNumPerfSlots && slots[(size_t) p].used)
                addPerf (slots[(size_t) p]);
    }
    return names;
}

void LiveSamplerProcessor::loadNeededInstruments()
{
    // prvo trenutni performans, pa ostatak banke (da prebacivanje bude trenutno)
    for (auto& n : neededInstruments (false))
        if (instrumentList.contains (n) && cache.count (n) == 0)
            requestInstrument (n, nullptr);
    for (auto& n : neededInstruments (true))
        if (instrumentList.contains (n) && cache.count (n) == 0)
            requestInstrument (n, nullptr);
}

InstrumentPtr LiveSamplerProcessor::getLoadedInstrument (const juce::String& name) const
{
    auto it = cache.find (name);
    return it != cache.end() ? it->second : nullptr;
}

void LiveSamplerProcessor::installInstrument (const juce::String& name, InstrumentPtr inst)
{
    if (inst == nullptr)
        return;
    cache[name] = inst;
    rebuildConfig();
    sendChangeMessage();
}

void LiveSamplerProcessor::applyZones (const juce::String& name, std::vector<Zone> zones)
{
    if (auto cur = getLoadedInstrument (name))
    {
        auto n = cur->withZones (std::move (zones));
        SampleLibrary::makeZonesPlayable (*n);   // loop / start iza dela u RAM-u: taj sempl se ucita ceo
        installInstrument (name, n);
    }
}

void LiveSamplerProcessor::setRoundRobinRandom (const juce::String& name, bool random)
{
    if (auto cur = getLoadedInstrument (name))
    {
        auto n = cur->withZones (cur->zones);
        n->rrRandom = random;
        installInstrument (name, n);
    }
}

bool LiveSamplerProcessor::saveMap (const juce::String& name)
{
    auto cur = getLoadedInstrument (name);
    if (cur == nullptr || ! cur->saveMap())
        return false;

    auto saved = cur->withZones (cur->zones);
    saved->fromMapFile = true;
    installInstrument (name, saved);
    return true;
}

void LiveSamplerProcessor::reloadInstrument (const juce::String& name, bool ignoreMapFile)
{
    if (auto it = cache.find (name); it != cache.end())
    {
        graveyard.push_back (it->second);
        cache.erase (it);
    }
    rebuildConfig();
    requestInstrument (name, [this, name] (InstrumentPtr inst) { installInstrument (name, inst); }, ignoreMapFile);
}

void LiveSamplerProcessor::requestInstrument (const juce::String& name, LoadCallback cb, bool ignoreMapFile)
{
    if (auto it = cache.find (name); it != cache.end())
    {
        if (cb) cb (it->second);
        return;
    }

    auto& waiting = pendingLoads[name];
    const bool alreadyLoading = ! waiting.empty();
    waiting.push_back (std::move (cb));
    if (alreadyLoading)
        return;

    const int epoch = loadEpoch;
    const bool c3 = c3Is60;
    auto aliveFlag = alive;
    loadPool.addJob ([this, name, epoch, c3, aliveFlag, ignoreMapFile]
    {
        if (! aliveFlag->load())
            return;

        auto inst = library.loadInstrument (name, c3, ignoreMapFile);

        juce::MessageManager::callAsync ([this, name, inst, epoch, aliveFlag]
        {
            if (aliveFlag->load())
                onInstrumentLoaded (name, inst, epoch);
        });
    });
    sendChangeMessage();
}

void LiveSamplerProcessor::onInstrumentLoaded (const juce::String& name, InstrumentPtr inst, int epoch)
{
    auto callbacks = std::move (pendingLoads[name]);
    pendingLoads.erase (name);

    if (epoch != loadEpoch)
    {
        graveyard.push_back (inst);
        return;
    }

    if (inst != nullptr)
    {
        if (! inst->warnings.isEmpty())
            lastWarning = name.fromLastOccurrenceOf ("/", false, false) + ": " + inst->warnings[0];
        if (inst->zones.empty())
            lastWarning = name + T (": no usable samples");
        cache[name] = inst;
    }

    for (auto& cb : callbacks)
        if (cb) cb (inst);

    rebuildConfig();
    sendChangeMessage();
}

//==============================================================================
//  Konfiguracija za audio nit
//==============================================================================
void LiveSamplerProcessor::rebuildConfig()
{
    auto cfg = std::make_shared<EngineConfig>();
    for (int i = 0; i < kNumParts; ++i)
    {
        auto& pr = cfg->parts[(size_t) i];
        pr.part = perf.parts[(size_t) i];
        const int u = findUser (pr.part.user);
        if (u >= 0)
        {
            pr.user = users[(size_t) u];
            for (auto& o : pr.user.osc)
                o.fineCents += pr.part.detuneCents;
            for (int o = 0; o < kNumOsc; ++o)
                if (pr.user.osc[(size_t) o].on && pr.user.osc[(size_t) o].instrument != Fm::instrumentName)
                    pr.inst[(size_t) o] = getLoadedInstrument (pr.user.osc[(size_t) o].instrument);
            if (pr.user.isFm())
                pr.fm = fmPatchFor (pr.user);
        }
        pr.active = pr.part.on && u >= 0;
        if (perf.tuneOn && pr.part.scaleTune) pr.scaleCents = perf.tuneCents;
        pr.maxVoices = maxPolyphony;
        // HARMONY Velocity: jacina terce (0.1 = oko -40 dB, 0.5 = -12 dB, 0.8 = -4 dB, 1.5 = +7 dB), boja ostaje kao kod melodije
        pr.harmGain = std::pow (juce::jlimit (0.05f, 1.5f, terca.velocity), 2.0f);
        pr.vectorOn = perf.vectorOn;
        pr.vectorEqualPower = perf.vectorEqualPower;
    }
    cfg->master = perf.master;
    cfg->globalDb = globalDb;
    cfg->globalEq = globalEq;
    cfg->globalTranspose = globalTranspose;
    cfg->perfSerial = perfSerial;
    cfg->terca = terca;

    ConfigPtr old;
    {
        const juce::SpinLock::ScopedLockType sl (configLock);
        old = pendingConfig;
        pendingConfig = cfg;
    }
    if (old != nullptr)
        configGraveyard.push_back (old);
}

LiveSamplerProcessor::AudioInfo LiveSamplerProcessor::getAudioInfo() const
{
    AudioInfo a;
    a.rate = getSampleRate();
    a.buffer = getBlockSize();
   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        auto& dm = holder->deviceManager;
        a.driver = dm.getCurrentAudioDeviceType();
        if (auto* dev = dm.getCurrentAudioDevice())
        {
            a.device = dev->getName();
            a.buffer = dev->getCurrentBufferSizeSamples();
            a.rate = dev->getCurrentSampleRate();
            if (a.rate > 0)
                a.latencyMs = 1000.0 * (a.buffer + dev->getOutputLatencyInSamples()) / a.rate;
        }
    }
   #endif
    if (a.latencyMs <= 0 && a.rate > 0) a.latencyMs = 1000.0 * a.buffer / a.rate;
    juce::StringArray adv;
   #if JUCE_WINDOWS
    if (a.driver.isNotEmpty() && a.driver != "ASIO")
        adv.add (T ("Driver is ") + a.driver + T (": choose ASIO in Options > Audio settings for low latency"));
   #endif
    if (a.latencyMs > 12.0) adv.add (T ("Latency over 12 ms: lower the buffer (128 or 256 samples)"));
    if (dropouts.load() > 0) adv.add (juce::String (dropouts.load()) + T (" audio dropouts: raise the buffer, or lower polyphony"));
    if (audioLoad.load() > 0.8f) adv.add (T ("Audio load is high (over 80%)"));
    if (SampleLibrary::streamingEnabled.load() && DiskStreamer::get().getUnderruns() > 0)
        adv.add (juce::String (DiskStreamer::get().getUnderruns()) + T (" disk underruns: the disk is too slow (use an SSD) or turn off disk streaming"));
    a.warn = ! adv.isEmpty();
    a.advice = adv.joinIntoString ("\n");
    return a;
}

juce::String LiveSamplerProcessor::getAudioShortText() const
{
    const auto a = getAudioInfo();
    juce::String s = "CPU " + juce::String (juce::roundToInt (audioLoad.load() * 100.0f)) + "%";
    if (a.latencyMs > 0) s << " | " << juce::String (a.latencyMs, 1) << " ms";
    if (a.driver.isNotEmpty()) s << " " << a.driver;
    if (dropouts.load() > 0) s << " | " << dropouts.load() << " drop";
    return s;
}

void LiveSamplerProcessor::setDiskStreaming (bool on)
{
    if (SampleLibrary::streamingEnabled.load() == on) return;
    if (on) DiskStreamer::get();
    SampleLibrary::streamingEnabled = on;
    juce::StringArray names;
    for (auto& kv : cache) names.add (kv.first);
    for (auto& n : names) reloadInstrument (n, false);
    markSave(); sendChangeMessage();
}

void LiveSamplerProcessor::keepMidiInputs()
{
   #if JucePlugin_Build_Standalone
    auto* holder = juce::StandalonePluginHolder::getInstance();
    if (holder == nullptr) return;
    auto& dm = holder->deviceManager;
    juce::StringArray present, enabled;
    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        present.add (d.name);
        if (dm.isMidiInputDeviceEnabled (d.identifier)) enabled.add (d.name);
    }
    bool changed = false;
    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        const bool on = enabled.contains (d.name), want = wantedMidiInputs.contains (d.name);
        if (on && ! want) { wantedMidiInputs.add (d.name); changed = true; }                       // korisnik ukljucio
        else if (! on && want && lastMidiPresent.contains (d.name) && lastMidiEnabled.contains (d.name))
        { wantedMidiInputs.removeString (d.name); changed = true; }                                // korisnik iskljucio
        else if (! on && want)
            dm.setMidiInputDeviceEnabled (d.identifier, true);                                     // port se pojavio: ukljuci ga
    }
    lastMidiPresent = present;
    lastMidiEnabled.clear();
    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (dm.isMidiInputDeviceEnabled (d.identifier)) lastMidiEnabled.add (d.name);
    if (changed) markSave();
   #endif
}

//==============================================================================
//  IMPULSE (iPad)
//==============================================================================
namespace
{
    struct ImpulseCb : juce::MidiInputCallback
    {
        LiveSamplerProcessor& p;
        explicit ImpulseCb (LiveSamplerProcessor& pp) : p (pp) {}
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override { p.impulseIncoming (m); }
    };

    std::vector<LiveSamplerProcessor::ImpulseCtl> impulseDefaults()
    {
        std::vector<LiveSamplerProcessor::ImpulseCtl> c;
        auto add = [&c] (const juce::String& id, const juce::String& kind, const juce::String& name, int type, int num, int ch)
        { LiveSamplerProcessor::ImpulseCtl x; x.id = id; x.kind = kind; x.name = name; x.type = type; x.num = num; x.ch = ch; c.push_back (x); };
        for (int i = 1; i <= 8; ++i) add ("k" + juce::String (i), "knob", "Knob " + juce::String (i), 1, 20 + i, 0);
        for (int i = 1; i <= 8; ++i) add ("f" + juce::String (i), "fader", "Fader " + juce::String (i), 1, 40 + i, 0);
        add ("f9", "fader", "Master", 1, 49, 0);
        for (int i = 1; i <= 9; ++i) add ("b" + juce::String (i), "btn", i == 9 ? "M" : juce::String (i), 1, 50 + i, 0);
        const char* tn[] = { "Rew", "Fwd", "Stop", "Play", "Loop", "Rec" };
        for (int i = 0; i < 6; ++i) add ("t" + juce::String (114 + i), "trans", tn[i], 1, 114 + i, 0);
        for (int i = 0; i < 8; ++i) add ("p" + juce::String (i + 1), "pad", "Pad " + juce::String (i + 1), 1, 80 + i, 0);   // Impulse padovi: CC 80-87
        add ("pb", "wheel", "Pitch", 3, 0, 0);
        add ("mw", "wheel", "Mod", 1, 1, 0);
        return c;
    }

    juce::String learnLabel (const juce::String& id)
    {
        // "perf.part3.volume" -> "Part 3 volume", "scene.2" -> "Scene 2", "live.harmony" -> "Harmony"
        auto s = id;
        for (auto* pre : { "perf.", "user.", "live.", "fx." }) if (s.startsWith (pre)) s = s.fromFirstOccurrenceOf (".", false, false);
        juce::String out;
        for (auto& w : juce::StringArray::fromTokens (s, ".", ""))
        {
            juce::String t = w;
            if (t.startsWith ("part") && t.substring (4).containsOnly ("0123456789")) t = "Part " + t.substring (4);
            else if (t.startsWith ("button") && t.substring (6).containsOnly ("0123456789")) t = "Button " + t.substring (6);
            else if (t.containsOnly ("0123456789")) t = t;
            out << (out.isEmpty() ? t.substring (0, 1).toUpperCase() + t.substring (1) : " " + t);
        }
        return out;
    }
}

void LiveSamplerProcessor::registerImpulseInputs()
{
   #if JucePlugin_Build_Standalone
    auto* holder = juce::StandalonePluginHolder::getInstance();
    if (holder == nullptr) return;
    if (impCallback == nullptr) impCallback = std::make_unique<ImpulseCb> (*this);
    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (d.name.containsIgnoreCase ("impulse") && ! impRegistered.contains (d.identifier))
        {
            holder->deviceManager.addMidiInputDeviceCallback (d.identifier, impCallback.get());
            impRegistered.add (d.identifier);
        }
   #endif
}

void LiveSamplerProcessor::impulseIncoming (const juce::MidiMessage& m)
{
    if (m.isController())
    {
        const int cc = m.getControllerNumber(), v = m.getControllerValue();
        const auto now = juce::Time::getMillisecondCounter();
        if (v >= 64) impDownMs[(size_t) cc] = now;
        else if (impCc[(size_t) cc].load() >= 64 && impDownMs[(size_t) cc] != 0 && now - impDownMs[(size_t) cc] < 350)
            impMomentary[(size_t) cc] = true;     // pusteno odmah posle pritiska: dugme je momentary (drzanje se racuna)
        impCc[(size_t) cc] = v;
        if (m.getChannel() != 10) impCh = m.getChannel();
    }
    else if (m.isNoteOnOrOff()) impNote[(size_t) m.getNoteNumber()] = m.isNoteOn() ? m.getVelocity() : 0;
    else if (m.isPitchWheel()) { impPb = m.getPitchWheelValue() >> 7; impCh = m.getChannel(); }
    else return;
    impSeen = true;
}

int LiveSamplerProcessor::getImpulseValue (const ImpulseCtl& c) const
{
    if (c.type == 3) return impPb.load();
    if (c.type == 2) return impNote[(size_t) juce::jlimit (0, 127, c.num)].load();
    return impCc[(size_t) juce::jlimit (0, 127, c.num)].load();
}

juce::String LiveSamplerProcessor::impulseTargetText (const ImpulseCtl& c, int& state) const
{
    state = -1;
    if (c.type == 3) return T ("Pitch bend");
    const int ch = c.ch > 0 ? c.ch : juce::jmax (1, impCh.load());
    const auto ids = MidiLearn::get().idsFor (c.type == 2 ? 2 : 1, ch, c.num);
    if (ids.isEmpty())
    {
        if (c.type == 1 && c.num == 1) return T ("Modulation");
        if (c.type == 1 && c.num == 11) return "Expression";
        if (c.type == 1 && c.num == 64) return "Sustain";
        if (c.type == 1 && (c.num == vectorCcX.load() || c.num == vectorCcY.load())) return "Vector " + juce::String (c.num == vectorCcX.load() ? "X" : "Y");
        return {};
    }
    juce::StringArray names;
    for (auto& id : ids)
    {
        names.add (learnLabel (id));
        bool st = false;
        if (MidiLearn::get().stateOf (id, st)) state = juce::jmax (state, st ? 1 : 0);
    }
    return names.joinIntoString (", ");
}

void LiveSamplerProcessor::loadImpulseLayout (const juce::var&)
{
    // raspored pravog Impulse-a (knobovi CC 21-28, faderi 41-49, dugmad 51-59, transport 114-119, padovi CC 80-87)
    auto def = impulseDefaults();
    impCtls = def;
    for (auto& x : impLatch) x = false;
    for (auto& c : impCtls)
        if (c.type == 1 && (c.kind == "btn" || c.kind == "trans")) impLatch[(size_t) juce::jlimit (0, 127, c.num)] = true;
}

void LiveSamplerProcessor::impulseFromRemote (const juce::String& id, int value)
{
    for (auto& c : impCtls)
    {
        if (c.id != id) continue;
        value = juce::jlimit (0, 127, value);
        const int ch = c.ch > 0 ? c.ch : juce::jmax (1, impCh.load());
        juce::uint32 msg = 0;
        if (c.type == 3)
        {
            const int w = juce::jlimit (0, 16383, value * 16383 / 127);
            msg = (juce::uint32) (0xE0 | (ch - 1)) | (juce::uint32) (w & 127) << 8 | (juce::uint32) (w >> 7) << 16;
            impPb = value;
        }
        else if (c.type == 2)
        {
            msg = (juce::uint32) ((value > 0 ? 0x90 : 0x80) | (ch - 1)) | (juce::uint32) c.num << 8 | (juce::uint32) (value > 0 ? value : 0) << 16;
            impNote[(size_t) c.num] = value;
        }
        else
        {
            msg = (juce::uint32) (0xB0 | (ch - 1)) | (juce::uint32) c.num << 8 | (juce::uint32) value << 16;
            impCc[(size_t) c.num] = value;
            // pravi knob / fader ostaje gde je bio: preuzima se tek kad stigne do ove vrednosti
            if (c.kind == "knob" || c.kind == "fader" || c.kind == "wheel") impPickup[(size_t) c.num] = value;
        }
        int s1, n1, s2, n2;
        impFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0) impFifoData[(size_t) s1] = msg;
        impFifo.finishedWrite (n1);
        return;
    }
}

void LiveSamplerProcessor::setMaxPolyphony (int n)
{
    maxPolyphony = juce::jlimit (8, 128, n);
    rebuildConfig(); markSave(); sendChangeMessage();
}

void LiveSamplerProcessor::setGigMode (bool on)
{
    gigMode = on;
    applyGigMode();
    markSave(); sendChangeMessage();
}

void LiveSamplerProcessor::applyGigMode()
{
    // GIG: ekran se ne gasi, program dobija visi prioritet (Windows: HIGH_PRIORITY_CLASS), manje crtanja u editoru
    juce::Desktop::setScreenSaverEnabled (! gigMode);
    juce::Process::setPriority (gigMode ? juce::Process::HighPriority : juce::Process::NormalPriority);
}

void LiveSamplerProcessor::timerCallback()
{
    if (const int kn = keyLearnNote.exchange (-1); kn >= 0 && keyLearnMode.load() != 0)
    {
        const int pi = juce::jlimit (0, kNumParts - 1, keyLearnPart.load());
        auto p = perf;
        if (keyLearnMode.load() == 2)
        {
            // SPLIT: ovaj part od tastera navise, ostali ukljuceni partovi koji su prelazili preko tastera se skracuju ispod njega
            p.parts[(size_t) pi].keyLow = kn;
            if (p.parts[(size_t) pi].keyHigh < kn) p.parts[(size_t) pi].keyHigh = 127;
            for (int i = 0; i < kNumParts; ++i)
            {
                auto& o = p.parts[(size_t) i];
                if (i == pi || ! o.on || o.user.isEmpty()) continue;
                if (o.keyLow < kn && o.keyHigh >= kn) o.keyHigh = kn - 1;
            }
            keyLearnMode = 0;
            updatePerformance (p);
        }
        else if (keyLearnFirst.load() < 0)
        {
            keyLearnFirst = kn;
            sendChangeMessage();
        }
        else
        {
            const int a = keyLearnFirst.load();
            p.parts[(size_t) pi].keyLow = juce::jmin (a, kn);
            p.parts[(size_t) pi].keyHigh = juce::jmax (a, kn);
            keyLearnMode = 0;
            keyLearnFirst = -1;
            updatePerformance (p);
        }
    }
    if (const int qm = qtMask.exchange (0); qm != 0)
    {
        auto p = perf;
        for (int pc = 0; pc < 12; ++pc)
            if (qm & (1 << pc))
                p.tuneCents[(size_t) pc] = std::abs (p.tuneCents[(size_t) pc]) > 0.5f ? 0.0f : -50.0f;
        p.tuneOn = true;
        p.tuneName = "Custom";
        updatePerformance (p);
    }
    // daljinsko upravljanje sa iPad-a: samo u samostalnom programu (ne u VST3 instanci)
    if (remote == nullptr && wrapperType == wrapperType_Standalone)
    {
        remote = std::make_unique<RemoteServer> (*this);
        if (gigMode) applyGigMode();
    }

    if (++midiTick >= 4) { midiTick = 0; keepMidiInputs(); registerImpulseInputs(); }

    // Live Kontroler: prvi put se sam poveze ako fajl postoji; posle se prati izmena fajla
    if (++kontrolerTick >= 4)
    {
        kontrolerTick = 0;
        if (! kontrolerChecked)
        {
            kontrolerChecked = true;
            if (defaultKontrolerFile().existsAsFile()) linkKontroler (defaultKontrolerFile());
        }
        else if (kontrolerLinked && kontrolerFile.existsAsFile() && kontrolerFile.getLastModificationTime() != kontrolerTime)
            syncKontroler();
    }
    // stare konfiguracije i instrumenti se oslobadjaju ovde, ne na audio niti
    configGraveyard.erase (std::remove_if (configGraveyard.begin(), configGraveyard.end(),
                                           [] (const ConfigPtr& p) { return p.use_count() == 1; }),
                           configGraveyard.end());

    auto keep = neededInstruments (true);
    for (auto it = cache.begin(); it != cache.end();)
    {
        if (! keep.contains (it->first) && it->second.use_count() == 1)
            it = cache.erase (it);
        else
            ++it;
    }
    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                     [] (const InstrumentPtr& p) { return p == nullptr || p.use_count() == 1; }),
                     graveyard.end());

    if (saveNeeded && juce::Time::getMillisecondCounter() - lastSaveCheck > 1500)
        saveNow();
    lastSaveCheck = juce::Time::getMillisecondCounter();
}

void LiveSamplerProcessor::handleAsyncUpdate()
{
    if (! isLearningVector()) { markSave(); }    // posle LEARN-a vector CC-a
    const int msb = pendingBankMsb.exchange (-1);
    if (msb >= 0)
    {
        if (pcMode == 1) setCurrentSetList (msb);
        else             setCurrentBank (msb);
    }
    const int prog = pendingProgram.exchange (-1);
    if (prog >= 0 && prog < 128)
    {
        if (pcMode == 1) recallSetListSlot (prog);
        else             recallPerformance (currentBank * kPerfPerBank + prog);
    }

    if (learnedType.load() != 0)
    {
        terca.learn.type = learnedType.exchange (0);
        terca.learn.channel = learnedChannel.load();
        terca.learn.number = learnedNumber.load();
        rebuildConfig();
        markSave();
    }

    if (tercaChanged.exchange (false))
        sendChangeMessage();
    sendChangeMessage();
}

//==============================================================================
//  Korg Kronos
//==============================================================================
bool LiveSamplerProcessor::loadKronosPcg (const juce::File& f, juce::String& error)
{
    auto pcg = std::make_unique<Kronos::Pcg>();
    if (! pcg->load (f, error))
        return false;

    kronosPcg = std::move (pcg);
    kronosFile = f;
    markSave();

    // folder sa PCG-om (i KSC kolekcijom pored njega) ide u biblioteku
    const auto dir = f.getParentDirectory();
    bool covered = false;
    for (auto& lf : libraryFolders)
        if (dir == lf || dir.isAChildOf (lf))
            covered = true;
    if (! covered)
        addLibraryFolder (dir);

    sendChangeMessage();
    return true;
}

namespace
{
    // Kronos vrednosti 0..99 -> nase jedinice (priblizno, po uhu i Korg tabelama)
    float kTime (int v)      { return v <= 0 ? 0.001f : 0.003f * std::exp (0.09f * (float) v); }
    float kHz (int v, float lo, float range) { return lo * std::pow (range, (float) juce::jlimit (0, 99, v) / 99.0f); }
    float kSemis (int v)     // intenzitet pitch-a: 60 = 1 poluton, 115 = 12
    {
        if (v == 0) return 0.0f;
        const float s = std::pow (2.0f, ((float) std::abs (v) - 60.0f) / 15.3f);
        return v < 0 ? -s : s;
    }
    // LFO: 40 = 1.7 Hz (izmereno na Kronosu: 17 titraja za 10 s), 0 = 0.03 Hz, 99 = 30 Hz
    float kLfoHz (int v)
    {
        const float x = (float) juce::jlimit (0, 99, v);
        return x <= 40.0f ? 0.03f * std::pow (1.7f / 0.03f, x / 40.0f)
                          : 1.7f * std::pow (30.0f / 1.7f, (x - 40.0f) / 59.0f);
    }
    float kLevelDb (int v)   { return v <= 0 ? -60.0f : 40.0f * std::log10 ((float) v / 127.0f); }

    EnvSettings kAmpEg (const std::array<int, 8>& e)
    {
        EnvSettings s;
        s.start = (float) e[0] / 99.0f;
        s.attackTime = kTime (e[1]);  s.attackLevel = (float) e[2] / 99.0f;
        s.decayTime = kTime (e[3]);   s.breakLevel = (float) e[4] / 99.0f;
        s.slopeTime = kTime (e[5]);   s.sustain = (float) e[6] / 99.0f;
        s.releaseTime = kTime (e[7]); s.releaseLevel = 0.0f;
        return s;
    }

    EnvSettings kFilterEg (const std::array<int, 9>& e)
    {
        EnvSettings s;
        s.start = (float) e[0] / 99.0f;
        s.attackTime = kTime (e[1]);  s.attackLevel = (float) e[2] / 99.0f;
        s.decayTime = kTime (e[3]);   s.breakLevel = (float) e[4] / 99.0f;
        s.slopeTime = kTime (e[5]);   s.sustain = (float) e[6] / 99.0f;
        s.releaseTime = kTime (e[7]); s.releaseLevel = (float) e[8] / 99.0f;
        return s;
    }
}

// Track EQ Kronos programa -> EQ usera (Kronos ima +-18 dB, ovde +-15)
static void applyKronosProgramEq (UserProgram& u, const Kronos::Program& prog)
{
    u.eqLowDb = juce::jlimit (-15.0f, 15.0f, prog.eqLowDb);
    u.eqMidDb = juce::jlimit (-15.0f, 15.0f, prog.eqMidDb);
    u.eqHighDb = juce::jlimit (-15.0f, 15.0f, prog.eqHighDb);
    u.eqMidFreq = juce::jlimit (100.0f, 10000.0f, prog.eqMidFreq);
}

int LiveSamplerProcessor::createUserFromKronos (const Kronos::Program& prog)
{
    if (prog.exi)
        return createUserFromKronosExi (prog);
    if (prog.drumKit)
        return createUserFromKronosDrum (prog);
    UserProgram u;
    int n = 0;
    const int numOsc = prog.doubleMode ? 2 : 1;

    for (int o = 0; o < numOsc; ++o)
    {
        const auto& q = prog.params[(size_t) o];
        auto& sb = u.block[(size_t) o];

        // zvuk (Zvuk A = OSC1, Zvuk B = OSC2)
        sb.ampEg = kAmpEg (q.ampEg);
        sb.filterType = (q.filterRouting & 3) == 3 || (q.filterRouting & 3) == 1 ? FilterType::LP24 : FilterType::LP12;
        sb.cutoff = kHz (q.cutoff, 20.0f, 1000.0f);
        sb.resonance = (float) juce::jlimit (0, 99, q.resonance) / 99.0f;
        // cutoff skala ima oko 10 oktava na 99 koraka, pa je jedan korak intenziteta ~0.1 oktave
        sb.filterEgInt = (float) q.filterEgInt * 0.1f;
        sb.velToFilterEg = (float) q.filterEgVel * 0.1f;
        sb.velSens = (float) juce::jlimit (0, 99, std::abs (q.ampVelocity)) / 99.0f;
        sb.filterEg = kFilterEg (q.filterEg);
        sb.vibRate = kLfoHz (q.lfo1Freq);
        sb.vibDepth = std::abs (kSemis (q.pitchLfo1Int)) * 100.0f;
        sb.mwToVib = std::abs (kSemis (q.pitchLfo1JsY)) * 100.0f;   // JS+Y = mod tockic
        sb.vibDelay = q.lfo1Delay <= 0 ? 0.0f : kTime (q.lfo1Delay);
        sb.vibFade = q.lfo1Fade <= 0 ? 0.005f : kTime (q.lfo1Fade);
        sb.vibOffset = juce::jlimit (-1.0f, 1.0f, (float) q.lfo1Offset / 99.0f);
        // pitch EG: nivoi jos nisu nadjeni u PCG-u, pa intenzitet ostaje 0 da ne pokvari visinu

        // zone -> oscilatori; zona 1 je najjaca, svaka sledeca ide ispod nje.
        // Xfade range ide od donje granice zone navise: gornja zona se tu pojacava,
        // donja se utisava (Linear/Power) ili svira punom jacinom (Layer).
        int top = 127, prevRange = 0, prevCurve = 0, ms = 0;
        for (int z = 0; z < 8 && top >= 1; ++z)
        {
            const auto& zone = prog.osc[(size_t) o][(size_t) z];
            if (! zone.used) break;
            const int bottom = juce::jmin (top, q.zoneBottom[(size_t) z]);
            const int range = q.xfadeRange[(size_t) z], curve = q.xfadeCurve[(size_t) z];
            const bool lowerExists = z < 7 && prog.osc[(size_t) o][(size_t) z + 1].used;
            const int fadeIn = range > 0 && lowerExists ? range : 0;
            const int fadeOut = prevRange > 0 && prevCurve != 2 ? prevRange : 0;
            const int lo = juce::jmax (bottom, q.velLow), hi = juce::jmin (top, q.velHigh);
            const int oscCurve = curve == 1 || prevCurve == 1 ? 1 : 0;

            top = bottom - 1 + (lowerExists ? range : 0);
            prevRange = lowerExists ? range : 0;
            prevCurve = curve;
            if (lo > hi || ms >= kMsPerOsc) continue;

            const auto inst = resolveKronosZone (zone);
            if (inst.isEmpty()) continue;

            auto& os = u.osc[(size_t) (o * kMsPerOsc + ms++)];
            ++n;
            os.on = true;
            os.instrument = inst;
            os.block = o;
            {
                // key off sempl (klik klapne harmonike, otpustanje zice...): svira kad se pusti taster (KO)
                const auto nm = inst.fromLastOccurrenceOf ("/", false, false).toLowerCase().removeCharacters (" _-");
                os.keyOff = nm.contains ("keyoff") || nm.contains ("releasenoise") || nm.contains ("relnoise") || nm.contains ("keyrelease");
            }
            os.velLow = lo;
            os.velHigh = hi;
            os.fadeIn = fadeIn;
            os.fadeOut = fadeOut;
            os.fadeCurve = oscCurve;
            os.keyLow = juce::jmin (q.keyLow, q.keyHigh);
            os.keyHigh = juce::jmax (q.keyLow, q.keyHigh);
            os.transpose = q.transpose + q.octave * 12;
            os.fineCents = (float) q.tuneCents;
            os.levelDb = juce::jlimit (-60.0f, 6.0f, kLevelDb (zone.value) + kLevelDb (q.ampLevel));
            os.pan = juce::jlimit (-1.0f, 1.0f, ((float) q.pan - 64.0f) / 64.0f);
        }
    }
    if (n == 0)
        return -1;

    const auto& q1 = prog.params[0];
    u.voiceMode = ! prog.mono ? VoiceMode::Poly : (prog.legato ? VoiceMode::MonoLegato : VoiceMode::Mono);
    u.priority = prog.priority == 0 ? Priority::Low : prog.priority == 1 ? Priority::High : Priority::Last;
    // send u master efekte: na L/R busu vaze sendovi OSC1; preko insert efekta (IFX) ostaje pun send
    u.send1 = prog.busLR ? (float) juce::jlimit (0, 127, q1.send1) / 127.0f : 1.0f;
    u.send2 = prog.busLR ? (float) juce::jlimit (0, 127, q1.send2) / 127.0f : 1.0f;
    u.portaOn = q1.portaOn != 0;
    u.portaFingered = q1.portaFingered != 0;
    u.portaConstRate = q1.portaConstTime == 0;
    u.portamento = q1.portaTime <= 0 ? 0.0f : 0.002f * std::exp (0.085f * (float) q1.portaTime);
    u.bendUp = juce::jlimit (0, 24, std::abs (q1.bendUp));
    u.bendDown = juce::jlimit (0, 24, std::abs (q1.bendDown));

    applyKronosProgramEq (u, prog);
    u.kronosPcg = kronosFile.getFileName();
    u.kronosSource = juce::String (prog.bankId) + ":" + juce::String (prog.number);
    const int idx = createUser (prog.name.isNotEmpty() ? prog.name : prog.id(), &u);
    loadNeededInstruments();
    for (auto& os : users[(size_t) idx].osc)
        if (os.on) requestInstrument (os.instrument, nullptr);
    return idx;
}

// EXi program je snimljen ceo (envelope, filter, LFO su vec u snimku): user samo svira snimak,
// a release i jacinu (velocity) uzima iz onoga sto je autosampler izmerio
int LiveSamplerProcessor::createUserFromKronosExi (const Kronos::Program& prog)
{
    const auto inst = resolveKronosExi (prog);
    if (inst.isEmpty())
        return -1;

    UserProgram u;
    auto& sb = u.block[0];
    sb.ampEg = EnvSettings();
    sb.ampEg.attackTime = 0.001f;
    sb.ampEg.sustain = 1.0f;
    const float rel = library.kronosTag (prog.exiKey, "release").getFloatValue();
    // izmereno je vreme do -30 dB; release ide do -60 dB ravnomerno po dB, pa je duplo
    sb.ampEg.releaseTime = juce::jlimit (0.05f, 20.0f, rel > 0.0f ? rel * 2.0f : 0.4f);
    sb.filterType = FilterType::Off;
    // vise velocity slojeva: dinamika je vec snimljena, pa jacina malo zavisi od udarca
    const int tagLayers = library.kronosTag (prog.exiKey, "layers").getIntValue();
    sb.velSens = tagLayers > 1 ? 0.25f : 0.6f;

    // svaki velocity sloj ide na svoj MS (MS1 = najjaci, kao na Kronosu), da moze posebno da se podesi ili obrise
    OscSettings proto;
    proto.on = true;
    proto.instrument = inst;
    proto.block = 0;
    const auto layers = velocityLayersOf (inst);
    if (layers.size() >= 2 && layers.size() <= (size_t) kMsPerOsc)
        layoutVelocityLayers (u, 0, proto, layers);
    else
        u.osc[0] = proto;

    u.voiceMode = ! prog.mono ? VoiceMode::Poly : (prog.legato ? VoiceMode::MonoLegato : VoiceMode::Mono);
    u.priority = prog.priority == 0 ? Priority::Low : prog.priority == 1 ? Priority::High : Priority::Last;
    u.send1 = prog.busLR ? (float) juce::jlimit (0, 127, prog.exiSend1) / 127.0f : 1.0f;
    u.send2 = prog.busLR ? (float) juce::jlimit (0, 127, prog.exiSend2) / 127.0f : 1.0f;

    // EQ programa je vec u snimku (autosampler snima ceo program), pa user ostaje bez EQ-a
    u.kronosPcg = kronosFile.getFileName();
    u.kronosSource = juce::String (prog.bankId) + ":" + juce::String (prog.number);
    const int idx = createUser (prog.name.isNotEmpty() ? prog.name : prog.id(), &u);
    loadNeededInstruments();
    requestInstrument (inst, nullptr);
    return idx;
}

// drum kit: svaki taster je snimljen posebno (svoja zona, bez pomeranja visine), mapa je u folderu
int LiveSamplerProcessor::createUserFromKronosDrum (const Kronos::Program& prog)
{
    const auto inst = resolveKronosDrum (prog);
    if (inst.isEmpty())
        return -1;
    UserProgram u;
    auto& sb = u.block[0];
    sb.ampEg = EnvSettings();
    sb.ampEg.attackTime = 0.001f;
    sb.ampEg.sustain = 1.0f;
    sb.ampEg.releaseTime = 0.3f;
    sb.filterType = FilterType::Off;
    const int tagLayers = library.kronosTag (prog.drumKey, "layers").getIntValue();
    sb.velSens = tagLayers > 1 ? 0.25f : 0.7f;
    u.osc[0].on = true;
    u.osc[0].instrument = inst;
    u.osc[0].block = 0;
    // EQ programa je vec u snimku (autosampler snima ceo program), pa user ostaje bez EQ-a
    u.kronosPcg = kronosFile.getFileName();
    u.kronosSource = juce::String (prog.bankId) + ":" + juce::String (prog.number);
    const int idx = createUser (prog.name.isNotEmpty() ? prog.name : prog.id(), &u);
    loadNeededInstruments();
    requestInstrument (inst, nullptr);
    return idx;
}

int LiveSamplerProcessor::fillMissingParts()
{
    // PCG fajlovi se citaju samo jednom po pozivu
    std::map<juce::String, std::unique_ptr<Kronos::Pcg>> pcgs;
    auto programFor = [&] (const PerfPart& pp) -> const Kronos::Program*
    {
        const juce::File f (pp.missingPcg);
        const Kronos::Pcg* pcg = nullptr;
        if (kronosPcg != nullptr && (kronosFile == f || (! f.existsAsFile() && kronosFile.getFileName() == f.getFileName()))) pcg = kronosPcg.get();
        else
        {
            auto& slot = pcgs[f.getFullPathName()];
            if (slot == nullptr && f.existsAsFile())
            {
                slot = std::make_unique<Kronos::Pcg>();
                juce::String err;
                if (! slot->load (f, err)) slot->programs.clear();
            }
            pcg = slot.get();
        }
        if (pcg == nullptr) return nullptr;
        const auto bank = (juce::uint32) pp.missingSource.upToFirstOccurrenceOf (":", false, false).getLargeIntValue();
        return pcg->findProgram (bank, pp.missingSource.fromFirstOccurrenceOf (":", false, false).getIntValue());
    };
    std::map<juce::String, juce::String> resolved;   // missingPcg|source -> ime usera ("" = jos nema)
    auto userFor = [&] (const PerfPart& pp) -> juce::String
    {
        const auto key = pp.missingPcg + "|" + pp.missingSource;
        if (auto it = resolved.find (key); it != resolved.end()) return it->second;
        juce::String name;
        if (auto* prog = programFor (pp))
            if (kronosProgramPlayable (*prog))
                if (const int u = userForKronosProgram (*prog); u >= 0) name = users[(size_t) u].name;
        resolved[key] = name;
        return name;
    };
    int filled = 0;
    for (auto& s : slots)
    {
        if (! s.used) continue;
        for (auto& pp : s.parts)
            if (pp.isMissing())
                if (auto nm = userFor (pp); nm.isNotEmpty()) { pp.user = nm; pp.clearMissing(); ++filled; }
    }
    bool perfChanged = false;
    for (auto& pp : perf.parts)
        if (pp.isMissing())
            if (auto nm = userFor (pp); nm.isNotEmpty()) { pp.user = nm; pp.clearMissing(); ++filled; perfChanged = true; }
    if (filled > 0)
    {
        markSave();
        if (perfChanged) { rebuildConfig(); loadNeededInstruments(); }
        sendChangeMessage();
    }
    return filled;
}

bool LiveSamplerProcessor::kronosFmCapable (const Kronos::Program& p)
{
    if (! p.exi || p.raw.size() < (size_t) Kronos::programSize) return false;
    const int a = p.exiAlg[0], b = p.exiAlg[1];
    if (! ((a == 7 && (b == 0 || b == 7)) || (a == 0 && b == 7))) return false;
    return kronosFmPatch (p).valid;
}

Fm::Patch LiveSamplerProcessor::kronosFmPatch (const Kronos::Program& p)
{
    if (p.raw.size() < (size_t) Kronos::programSize) return {};
    const int slot = p.exiAlg[0] == 7 ? 0 : 1;
    return Fm::Patch::fromMod7 (p.raw.data() + (slot == 0 ? 2908 : 3960), p.raw.data(), slot);
}

std::shared_ptr<const Fm::Patch> LiveSamplerProcessor::fmPatchFor (const UserProgram& u)
{
    const auto key = u.fmPatch.isNotEmpty() ? u.fmPatch.hashCode64() : u.fmProgram.hashCode64() ^ (juce::int64) u.fmSlot;
    if (auto it = fmCache.find (key); it != fmCache.end()) return it->second;
    if (fmCache.size() > 300) fmCache.clear();   // editovanje pravi nove verzije
    if (u.fmPatch.isNotEmpty())
    {
        auto patch = std::make_shared<const Fm::Patch> (Fm::Patch::fromString (u.fmPatch));
        fmCache[key] = patch;
        return patch;
    }
    juce::MemoryBlock mb;
    mb.loadFromHexString (u.fmProgram);
    std::shared_ptr<const Fm::Patch> patch;
    if (mb.getSize() >= (size_t) Kronos::programSize)
    {
        auto* d = (const juce::uint8*) mb.getData();
        patch = std::make_shared<const Fm::Patch> (Fm::Patch::fromMod7 (d + (u.fmSlot == 0 ? 2908 : 3960), d, u.fmSlot));
    }
    fmCache[key] = patch;
    return patch;
}

int LiveSamplerProcessor::createUserFromKronosFm (const Kronos::Program& prog)
{
    if (! kronosFmCapable (prog)) return -1;
    UserProgram u;
    u.fmPatch = kronosFmPatch (prog).toString();
    auto& sb = u.block[0];
    sb.ampEg = EnvSettings();          // jacinu i release vodi Amp EG MOD-7 programa
    sb.ampEg.attackTime = 0.001f;
    sb.ampEg.sustain = 1.0f;
    sb.ampEg.releaseTime = 30.0f;
    sb.filterType = FilterType::Off;
    sb.velSens = 0.0f;                  // dinamika je u FM programu
    u.osc[0].on = true;
    u.osc[0].instrument = Fm::instrumentName;
    u.osc[0].block = 0;
    u.voiceMode = ! prog.mono ? VoiceMode::Poly : (prog.legato ? VoiceMode::MonoLegato : VoiceMode::Mono);
    u.priority = prog.priority == 0 ? Priority::Low : prog.priority == 1 ? Priority::High : Priority::Last;
    u.send1 = prog.busLR ? (float) juce::jlimit (0, 127, prog.exiSend1) / 127.0f : 1.0f;
    u.send2 = prog.busLR ? (float) juce::jlimit (0, 127, prog.exiSend2) / 127.0f : 1.0f;
    applyKronosProgramEq (u, prog);    // EQ nije u snimku: svira ga Workstation
    u.category = "Synth Lead";
    u.kronosPcg = kronosFile.getFileName();
    u.kronosSource = juce::String (prog.bankId) + ":" + juce::String (prog.number);
    return createUser ((prog.name.isNotEmpty() ? prog.name : prog.id()) + " [FM]", &u);
}

int LiveSamplerProcessor::createFmUser()
{
    UserProgram u;
    u.fmPatch = Fm::Patch::makeInit().toString();
    auto& sb = u.block[0];
    sb.ampEg = EnvSettings();
    sb.ampEg.attackTime = 0.001f;
    sb.ampEg.sustain = 1.0f;
    sb.ampEg.releaseTime = 30.0f;
    sb.filterType = FilterType::Off;
    sb.velSens = 0.0f;
    u.osc[0].on = true;
    u.osc[0].instrument = Fm::instrumentName;
    u.osc[0].block = 0;
    u.category = "Synth Lead";
    return createUser ("FM User", &u);
}

juce::StringArray LiveSamplerProcessor::userUsage (const juce::String& name) const
{
    juce::StringArray out;
    if (name.isEmpty()) return out;
    for (int s = 0; s < kNumPerfSlots; ++s)
    {
        const auto& sl = slots[(size_t) s];
        if (! sl.used) continue;
        for (auto& p : sl.parts)
            if (p.user == name)
            {
                out.add (juce::String (s / kPerfPerBank + 1) + "-" + juce::String (s % kPerfPerBank).paddedLeft ('0', 3) + "  " + sl.name);
                break;
            }
    }
    if (currentSlot < 0)
        for (auto& p : perf.parts)
            if (p.user == name) { out.add ("(current, not saved)  " + perf.name); break; }
    return out;
}

int LiveSamplerProcessor::userForKronosProgram (const Kronos::Program& p)
{
    // user napravljen od istog programa (i kad je posle preimenovan); FM ili semplovi, sta je izabrano
    const auto src = juce::String (p.bankId) + ":" + juce::String (p.number);
    const bool fm = kronosFmCapable (p) && (preferFm || (p.exi && resolveKronosExi (p).isEmpty()));
    for (size_t i = 0; i < users.size(); ++i)
        if (users[i].kronosSource == src && users[i].kronosPcg == kronosFile.getFileName() && users[i].isFm() == fm)
            return (int) i;
    if (fm)
        return createUserFromKronosFm (p);
    const int existing = findUser (p.name);
    if (existing >= 0)
        return existing;
    return createUserFromKronos (p);
}

juce::StringArray LiveSamplerProcessor::loadKronosCombi (const Kronos::Combi& c, bool keepChannels, std::vector<KronosMissing>* missing)
{
    juce::StringArray skipped;
    if (kronosPcg == nullptr)
        return skipped;

    auto p = perf;
    p.name = c.name.isNotEmpty() ? c.name : c.id();
    p.kronosSource = juce::String (c.bankId) + ":" + juce::String (c.number);
    p.kronosPcg = kronosFile.getFileName();
    p.vectorOn = c.vectorVolume;
    p.vectorEqualPower = c.vectorEqualPower;
    p.category = {};
    p.master.reverbLevel = (float) c.return1 / 127.0f;   // MFX1 Return
    p.master.delayLevel = (float) c.return2 / 127.0f;    // MFX2 Return
    for (int t = 0; t < 16 && t < kNumParts; ++t)
    {
        const auto& tm = c.timbres[(size_t) t];
        auto& part = p.parts[(size_t) t];
        part = PerfPart();
        if (! tm.play)
            continue;

        const auto* prog = kronosPcg->findProgram (tm.programBank, tm.programNumber);
        const int u = prog != nullptr && kronosProgramPlayable (*prog) ? userForKronosProgram (*prog) : -1;
        part.on = true;
        if (u < 0)
        {
            const auto what = prog != nullptr && prog->exi ? " (EXi " + prog->engineText() + juce::String::fromUTF8 (" synthesis, not resampled)")
                            : prog != nullptr && prog->drumKit ? T (" (drum kit, not resampled)")
                            : prog != nullptr && ! prog->usesUser() ? T (" (factory ROM, not resampled)")
                            : T (" (samples not in the library)");
            skipped.add ("Timbre " + juce::String (t + 1) + ": "
                         + (prog != nullptr ? prog->id() + " " + prog->name : juce::String ("program ") + juce::String (tm.programNumber)) + what);
            if (prog == nullptr)
            {
                part.on = false;
                continue;
            }
            // part ostaje (sa svim podesavanjima), ceka presemplovan zvuk ili zamenu
            part.missingName = prog->id() + " " + prog->name;
            part.missingSource = juce::String (prog->bankId) + ":" + juce::String (prog->number);
            part.missingPcg = kronosFile.getFullPathName();
            if (missing != nullptr)
            {
                bool rom = prog->exi || prog->drumKit;
                for (auto& o : prog->osc) for (auto& z : o) rom = rom || z.romKey.isNotEmpty();
                bool have = false;
                for (auto& m : *missing) if (m.bank == prog->bankId && m.number == prog->number) { have = true; m.where << ", " << c.name << " T" << (t + 1); }
                if (! have)
                    missing->push_back ({ prog->bankId, prog->number, prog->id() + " " + prog->name + what, c.name + " T" + juce::String (t + 1), rom });
            }
        }
        else
            part.user = users[(size_t) u].name;
        part.volumeDb = tm.volume <= 0 ? -60.0f : juce::jlimit (-60.0f, 6.0f, 40.0f * std::log10 ((float) tm.volume / 127.0f));
        part.pan = juce::jlimit (-1.0f, 1.0f, ((float) tm.pan - 64.0f) / 64.0f);
        part.transpose = tm.transpose;
        part.detuneCents = (float) tm.detuneCents;
        part.keyLow = juce::jmin (tm.keyLow, tm.keyHigh);
        part.vectorDir = c.vectorVolume ? tm.vectorDir : 0;
        part.sustainPedal = tm.damper;       // MIDI Filter u kombiju
        part.expression = tm.footPedal;
        part.vectorCenter = (float) tm.vectorCenter * 0.25f;
        part.keyHigh = juce::jmax (tm.keyLow, tm.keyHigh);
        part.velLow = juce::jmin (tm.velLow, tm.velHigh);
        part.velHigh = juce::jmax (tm.velLow, tm.velHigh);
        // timbre na globalnom kanalu Kronosa svira sa klavijature kao Gch -> svi kanali.
        // Drum kit na svom kanalu (npr. 10) ostaje na tom kanalu, da ne svira sa klavijature preko ostalih zvukova.
        const bool drumOwnChannel = prog != nullptr && prog->drumKit && tm.channel != 0 && tm.channel != kronosPcg->globalChannel;
        part.midiChannel = (! keepChannels && ! drumOwnChannel) || tm.channel == kronosPcg->globalChannel ? 0 : tm.channel;
        part.reverbSend = (float) tm.send1 / 127.0f;
        part.delaySend = (float) tm.send2 / 127.0f;
        // U kombiju svira Track EQ timbre-a umesto EQ-a programa. EQ parta se dodaje na EQ usera,
        // pa part dobija razliku (timbre - user). Za zvuk koji fali: razlika prema EQ-u programa,
        // jer ce presemplovan user dobiti bas taj EQ.
        {
            float bl = prog != nullptr ? prog->eqLowDb : 0.0f, bm = prog != nullptr ? prog->eqMidDb : 0.0f, bh = prog != nullptr ? prog->eqHighDb : 0.0f;
            if (u >= 0)
            {
                // HD-1: EQ programa je u useru. EXi / drum kit: EQ programa je vec u snimku (+ eventualni EQ usera)
                const auto& us = users[(size_t) u];
                const bool recorded = (prog->exi || prog->drumKit) && ! us.isFm();
                bl = us.eqLowDb + (recorded ? prog->eqLowDb : 0.0f);
                bm = us.eqMidDb + (recorded ? prog->eqMidDb : 0.0f);
                bh = us.eqHighDb + (recorded ? prog->eqHighDb : 0.0f);
            }
            auto d = [] (float a, float b) { const float v = juce::jlimit (-15.0f, 15.0f, a - b); return std::abs (v) < 0.05f ? 0.0f : v; };
            part.eqLowDb = d (tm.eqLowDb, bl);
            part.eqMidDb = d (tm.eqMidDb, bm);
            part.eqHighDb = d (tm.eqHighDb, bh);
        }
    }
    updatePerformance (p);
    return skipped;
}

//==============================================================================
//  Useri
//==============================================================================
int LiveSamplerProcessor::findUser (const juce::String& name) const
{
    if (name.isEmpty())
        return -1;
    for (size_t i = 0; i < users.size(); ++i)
        if (users[i].name == name)
            return (int) i;
    return -1;
}

juce::StringArray LiveSamplerProcessor::getUserNames() const
{
    juce::StringArray s;
    for (auto& u : users)
        s.add (u.name);
    return s;
}

juce::String LiveSamplerProcessor::uniqueUserName (const juce::String& base, int ignoreIndex) const
{
    auto clean = base.trim().isEmpty() ? juce::String ("User") : base.trim();
    auto taken = [this, ignoreIndex] (const juce::String& n)
    {
        for (size_t i = 0; i < users.size(); ++i)
            if ((int) i != ignoreIndex && users[i].name.equalsIgnoreCase (n))
                return true;
        return false;
    };
    if (! taken (clean))
        return clean;
    for (int k = 2;; ++k)
        if (! taken (clean + " " + juce::String (k)))
            return clean + " " + juce::String (k);
}

int LiveSamplerProcessor::createUser (const juce::String& baseName, const UserProgram* copyFrom)
{
    UserProgram u = copyFrom != nullptr ? *copyFrom : UserProgram();
    u.name = uniqueUserName (baseName);
    users.push_back (u);
    markSave();
    sendChangeMessage();
    return (int) users.size() - 1;
}

int LiveSamplerProcessor::createUserFromInstrument (const juce::String& instrument)
{
    UserProgram u;
    u.osc[0].on = true;
    u.osc[0].instrument = instrument;
    const int i = createUser (instrument.fromLastOccurrenceOf ("/", false, false), &u);
    loadNeededInstruments();
    requestInstrument (instrument, nullptr);
    return i;
}

void LiveSamplerProcessor::updateUser (int index, const UserProgram& u)
{
    if (index < 0 || index >= (int) users.size())
        return;

    auto updated = u;
    updated.name = uniqueUserName (u.name, index);
    const auto oldName = users[(size_t) index].name;

    // preimenovanje: popravi reference u performansima
    if (oldName != updated.name)
    {
        for (auto& p : perf.parts)
            if (p.user == oldName) p.user = updated.name;
        for (auto& s : slots)
            for (auto& p : s.parts)
                if (p.user == oldName) p.user = updated.name;
    }

    users[(size_t) index] = updated;
    markSave();
    rebuildConfig();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::deleteUser (int index)
{
    if (index < 0 || index >= (int) users.size())
        return;
    users.erase (users.begin() + index);
    markSave();
    rebuildConfig();
    sendChangeMessage();
}

void LiveSamplerProcessor::moveUser (int from, int to)
{
    const int n = (int) users.size();
    if (from < 0 || from >= n) return;
    to = juce::jlimit (0, n - 1, to);
    if (from == to) return;
    auto u = users[(size_t) from];
    users.erase (users.begin() + from);
    users.insert (users.begin() + to, u);
    markSave();
    rebuildConfig();
    sendChangeMessage();
}

void LiveSamplerProcessor::editUserCategory (const juce::String& oldName, const juce::String& newName)
{
    const auto nn = newName.trim();
    if (oldName.isEmpty())                       // nova
    {
        if (nn.isNotEmpty()) userCategories.addIfNotAlreadyThere (nn);
    }
    else
    {
        const int k = userCategories.indexOf (oldName);
        if (k < 0) return;
        if (nn.isEmpty()) userCategories.remove (k);   // brisanje
        else userCategories.set (k, nn);
        for (auto& u : users)
            if (u.category == oldName) u.category = nn;
    }
    markSave();
    sendChangeMessage();
}

//==============================================================================
//  Performans
//==============================================================================
void LiveSamplerProcessor::updatePerformance (const Performance& p)
{
    perf = p;
    for (auto& pp : perf.parts)
        if (pp.user.isNotEmpty() && pp.missingSource.isNotEmpty()) pp.clearMissing();   // zamenjen drugim userom
    perfDirty = true;
    markSave();
    rebuildConfig();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::storePerformance (int slot)
{
    if (slot < 0 || slot >= kNumPerfSlots)
        return;
    perf.used = true;
    slots[(size_t) slot] = perf;
    currentSlot = slot;
    perfDirty = false;
    markSave();
    sendChangeMessage();
    kontrolerChanged();
}

void LiveSamplerProcessor::recallPerformance (int slot)
{
    if (slot < 0 || slot >= kNumPerfSlots || ! slots[(size_t) slot].used)
        return;
    perf = slots[(size_t) slot];
    currentSlot = slot;
    currentBank = slot / kPerfPerBank;
    ++perfSerial;     // zvuci prethodnog performansa izdisu prirodno (SST)
    vectorX = 0.0f; vectorY = 0.0f;     // joystick krece iz centra, kao na Kronosu
    vectorUiDirty = true;
    perfDirty = false;
    markSave();
    rebuildConfig();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::setTuning (bool on)
{
    auto p = perf;
    p.tuneOn = on;
    updatePerformance (p);
}

void LiveSamplerProcessor::setTuningPreset (int preset, int root)
{
    const auto& ps = Tuning::presets();
    if (preset < 0 || preset >= (int) ps.size()) return;
    auto p = perf;
    p.tuneCents = Tuning::apply (ps[(size_t) preset], juce::jlimit (0, 11, root));
    p.tuneName = juce::String (ps[(size_t) preset].name) + (preset > 0 ? " " + Terca::pcName (root) : juce::String());
    p.tuneOn = preset > 0;
    updatePerformance (p);
}

void LiveSamplerProcessor::setTuneCents (int pc, float cents)
{
    if (pc < 0 || pc > 11) return;
    auto p = perf;
    p.tuneCents[(size_t) pc] = juce::jlimit (-100.0f, 100.0f, cents);
    p.tuneName = "Custom";
    p.tuneOn = true;
    updatePerformance (p);
}

void LiveSamplerProcessor::stepPerformance (int dir)
{
    // sledeci / prethodni zauzet performans u istoj listi
    dir = dir >= 0 ? 1 : -1;
    int s = currentSlot;
    const int bank = s >= 0 ? s / kPerfPerBank : currentBank;
    if (s < 0) s = bank * kPerfPerBank - dir;
    for (int k = 0; k < kPerfPerBank; ++k)
    {
        s += dir;
        if (s < bank * kPerfPerBank || s >= (bank + 1) * kPerfPerBank) break;
        if (slots[(size_t) s].used) { recallPerformance (s); break; }
    }
}

void LiveSamplerProcessor::recallScene (int s)
{
    if (s < 0 || s >= kNumScenes || ! perf.scenes[(size_t) s].used) return;
    const auto& sc = perf.scenes[(size_t) s];
    for (int i = 0; i < kNumParts; ++i)
    {
        const auto& a = sc.parts[(size_t) i];
        auto& p = perf.parts[(size_t) i];
        p.on = a.on && (p.user.isNotEmpty() || p.isMissing());
        p.volumeDb = a.volumeDb; p.pan = a.pan;
        p.transpose = a.transpose; p.octave = a.octave; p.terca = a.terca;
        p.keyLow = a.keyLow; p.keyHigh = a.keyHigh;
        p.reverbSend = a.reverb; p.delaySend = a.delay;
    }
    perf.scene = s;
    // scena nije izmena performansa (kao na Montage-u): ne pali "*"
    markSave();
    rebuildConfig();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::storeScene (int s)
{
    if (s < 0 || s >= kNumScenes) return;
    auto& sc = perf.scenes[(size_t) s];
    sc.used = true;
    if (sc.name.isEmpty()) sc.name = "Scene " + juce::String (s + 1);
    for (int i = 0; i < kNumParts; ++i)
    {
        const auto& p = perf.parts[(size_t) i];
        auto& a = sc.parts[(size_t) i];
        a.on = p.on; a.volumeDb = p.volumeDb; a.pan = p.pan;
        a.transpose = p.transpose; a.octave = p.octave; a.terca = p.terca;
        a.keyLow = p.keyLow; a.keyHigh = p.keyHigh;
        a.reverb = p.reverbSend; a.delay = p.delaySend;
    }
    perf.scene = s;
    // scena se odmah upisuje i u mesto u listi (ostale izmene performansa cekaju SAVE)
    if (currentSlot >= 0 && slots[(size_t) currentSlot].used)
        slots[(size_t) currentSlot].scenes[(size_t) s] = sc;
    markSave();
    sendChangeMessage();
}

void LiveSamplerProcessor::clearScene (int s)
{
    if (s < 0 || s >= kNumScenes) return;
    perf.scenes[(size_t) s] = Scene();
    if (perf.scene == s) perf.scene = -1;
    if (currentSlot >= 0) slots[(size_t) currentSlot].scenes[(size_t) s] = Scene();
    markSave();
    sendChangeMessage();
}

void LiveSamplerProcessor::renameScene (int s, const juce::String& name)
{
    if (s < 0 || s >= kNumScenes || ! perf.scenes[(size_t) s].used) return;
    perf.scenes[(size_t) s].name = name.trim().substring (0, 24);
    if (currentSlot >= 0) slots[(size_t) currentSlot].scenes[(size_t) s].name = perf.scenes[(size_t) s].name;
    markSave();
    sendChangeMessage();
}

void LiveSamplerProcessor::initPerformanceSlot (int slot)
{
    if (slot < 0 || slot >= kNumPerfSlots) return;
    auto p = Performance::makeDefault (users.empty() ? juce::String() : users[0].name);
    p.name = "Init";
    p.used = true;
    slots[(size_t) slot] = p;
    markSave();
    sendChangeMessage();
    kontrolerChanged();
}

std::vector<int> LiveSamplerProcessor::velocityLayersOf (const juce::String& instrument) const
{
    std::vector<int> out;
    const auto dir = library.instrumentFile (instrument);
    if (! dir.isDirectory()) return out;
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        int note = -1, layer = 0;
        if (SampleLibrary::parseFileName (f.getFileNameWithoutExtension(), true, note, layer) && layer > 0 && layer <= 127
            && std::find (out.begin(), out.end(), layer) == out.end())
            out.push_back (layer);
    }
    std::sort (out.begin(), out.end(), std::greater<int>());
    if (! out.empty() && out.front() <= 16) out.clear();     // brojevi slojeva (v1, v2...), ne jacine
    return out;
}

bool LiveSamplerProcessor::splitVelocityLayers (int userIndex, int oscIndex, juce::String& report)
{
    if (userIndex < 0 || userIndex >= (int) users.size() || oscIndex < 0 || oscIndex >= kNumOsc) return false;
    auto user = users[(size_t) userIndex];
    const auto src = user.osc[(size_t) oscIndex];
    const auto layers = velocityLayersOf (src.instrument);
    if (layers.size() < 2) { report = T ("This sound has only one velocity layer."); return false; }
    if (layers.size() > (size_t) kMsPerOsc) { report = T ("Too many layers for one OSC (max 8)."); return false; }
    const int block = oscIndex / kMsPerOsc;
    for (int m = 0; m < kMsPerOsc; ++m)
    {
        const auto& o = user.osc[(size_t) (block * kMsPerOsc + m)];
        if (block * kMsPerOsc + m != oscIndex && o.on && o.instrument != src.instrument)
        {
            report = T ("This OSC has other MS too. Split the layers in an empty OSC (copy the MS there).");
            return false;
        }
    }
    const bool matched = layoutVelocityLayers (user, block, src, layers);
    updateUser (userIndex, user);
    report = juce::String ((int) layers.size()) + T (" velocity layers are now MS1-MS") + juce::String ((int) layers.size()) + T (" (MS1 = loudest), with crossfades at the borders")
           + (matched ? T (" and matched dynamics (level rises evenly from the soft to the loud layer).") : T ("."));
    return true;
}

// glasnoca svakog sloja (dB): RMS prvih 0.3 s od pocetka tona, srednja vrednost (medijana) po svim tonovima
std::vector<float> LiveSamplerProcessor::measureLayerLevels (const juce::String& instrument, const std::vector<int>& layers) const
{
    std::vector<float> out (layers.size(), 0.0f);
    const auto dir = library.instrumentFile (instrument);
    if (! dir.isDirectory()) return {};
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::map<int, std::vector<float>> per;
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        int note = -1, layer = 0;
        if (! SampleLibrary::parseFileName (f.getFileNameWithoutExtension(), true, note, layer)) continue;
        if (std::find (layers.begin(), layers.end(), layer) == layers.end()) continue;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) continue;
        const int len = (int) juce::jmin ((juce::int64) (r->sampleRate * 1.5), r->lengthInSamples);
        juce::AudioBuffer<float> b (2, len);
        r->read (&b, 0, len, 0, true, true);
        const int on = AutoLoop::findOnset (b, len, r->sampleRate);
        const int n = juce::jmin (len - on, (int) (r->sampleRate * 0.3));
        if (n <= 0) continue;
        const float rms = 0.5f * (b.getRMSLevel (0, on, n) + b.getRMSLevel (1, on, n));
        if (rms > 1.0e-6f) per[layer].push_back (juce::Decibels::gainToDecibels (rms));
    }
    for (size_t k = 0; k < layers.size(); ++k)
    {
        auto& v = per[layers[k]];
        if (v.empty()) return {};
        std::sort (v.begin(), v.end());
        out[k] = v[v.size() / 2];
    }
    return out;
}

// slojevi (jacine od najjace) -> MS1..MSn u datom OSC-u: pretapanje na granicama i izjednacena dinamika.
// Unutar sloja jacina raste od nivoa slabijeg sloja do svog nivoa, pa nema skoka ni duplog utisavanja tihih slojeva.
bool LiveSamplerProcessor::layoutVelocityLayers (UserProgram& user, int block, const OscSettings& proto, const std::vector<int>& layers)
{
    const int n = (int) layers.size();
    const auto levels = measureLayerLevels (proto.instrument, layers);
    bool matched = (int) levels.size() == n;
    for (int k = 0; k + 1 < n && matched; ++k)
        if (levels[(size_t) k] <= levels[(size_t) k + 1]) matched = false;   // jaci sloj mora biti glasniji
    int minRange = 127;
    for (int k = 0; k < n; ++k)
        minRange = juce::jmin (minRange, (k == 0 ? 127 : layers[(size_t) k]) - (k + 1 < n ? layers[(size_t) k + 1] : 0));
    const int xf = juce::jlimit (0, 8, minRange / 3);   // pretapanje (koraka jacine)

    for (int m = 0; m < kMsPerOsc; ++m)
    {
        auto& o = user.osc[(size_t) (block * kMsPerOsc + m)];
        if (m >= n) { o = OscSettings(); o.block = block; continue; }
        o = proto;
        o.block = block;
        o.velHigh = m == 0 ? 127 : layers[(size_t) m];
        o.velLow = m + 1 < n ? layers[(size_t) m + 1] + 1 : 1;
        o.fadeIn = m + 1 < n ? xf : 0;
        o.fadeOut = m > 0 ? xf : 0;
        if (m > 0) o.velHigh = juce::jmin (127, o.velHigh + xf);   // slabiji sloj ulazi u pretapanje
        o.fadeCurve = xf > 0 ? 1 : 0;
        o.velRampDb = 0.0f;
        if (matched)
        {
            if (m + 1 < n) o.velRampDb = juce::jlimit (-24.0f, 0.0f, levels[(size_t) m + 1] - levels[(size_t) m]);
            else if (n >= 2)
            {
                // najtiši sloj: nastavi istim nagibom kao izmedju poslednja dva sloja
                const float perStep = (levels[(size_t) n - 2] - levels[(size_t) n - 1]) / (float) juce::jmax (1, layers[(size_t) n - 2] - layers[(size_t) n - 1]);
                o.velRampDb = juce::jlimit (-24.0f, 0.0f, -perStep * (float) (layers[(size_t) n - 1] - 1));
            }
        }
    }
    // dinamika je u slojevima i u izjednacenju: opsta osetljivost bloka bi tihe slojeve utisala jos jednom
    if (matched) user.block[(size_t) block].velSens = 0.0f;
    return matched;
}

bool LiveSamplerProcessor::deleteVelocityLayer (int userIndex, int oscIndex, juce::String& report)
{
    if (userIndex < 0 || userIndex >= (int) users.size() || oscIndex < 0 || oscIndex >= kNumOsc) return false;
    const auto os = users[(size_t) userIndex].osc[(size_t) oscIndex];
    const auto dir = library.instrumentFile (os.instrument);
    const auto layers = velocityLayersOf (os.instrument);
    if (! dir.isDirectory() || layers.size() < 2)
    {
        report = T ("This MS has no separate velocity layers (needs a resampled sound with files \"060 v127.wav\").");
        return false;
    }
    // sloj ovog MS-a: jacina snimka unutar opsega MS-a
    int layer = -1, inRange = 0;
    for (int v : layers) if (v >= os.velLow && v <= os.velHigh) { if (layer < 0) layer = v; ++inRange; }
    if (inRange > 1)
    {
        report = T ("This MS plays several layers at once. First choose \"Split layers to MS\", then delete the layer you don't need.");
        return false;
    }
    if (layer < 0) { report = T ("The range of this MS does not match any recorded layer."); return false; }
    const auto idx = (size_t) (std::find (layers.begin(), layers.end(), layer) - layers.begin());
    // opseg sloja u fajlovima: od (sledeci slabiji + 1) do ove jacine (najjaci do 127)
    const int lo = idx + 1 < layers.size() ? layers[idx + 1] + 1 : 1;
    const int hi = idx == 0 ? 127 : layer;
    const bool top = idx == 0;

    // 1) fajlovi tog sloja
    int deleted = 0;
    juce::StringArray removedIds;
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        int note = -1, l = 0;
        if (SampleLibrary::parseFileName (f.getFileNameWithoutExtension(), true, note, l) && l == layer)
        {
            removedIds.add (f.getFileName());
            if (f.deleteFile()) ++deleted;
        }
    }
    // 2) sacuvana mapa: zone tog sloja nestaju, susedni sloj preuzima opseg
    //    (brise se najjaci -> slabiji ide do 127; inace jaci sloj ide nanize)
    const auto mapFile = dir.getChildFile ("LiveSampler-mapa.xml");
    if (auto xml = juce::XmlDocument::parse (mapFile))
    {
        for (int i = xml->getNumChildElements(); --i >= 0;)
        {
            auto* z = xml->getChildElement (i);
            const auto id = z->getStringAttribute ("sample");
            bool gone = false;
            for (auto& r : removedIds) gone = gone || id.endsWithIgnoreCase (r) || id.endsWithIgnoreCase (juce::File (r).getFileNameWithoutExtension());
            if (gone) { xml->removeChildElement (z, true); continue; }
            if (top && z->getIntAttribute ("highVel") == lo - 1) z->setAttribute ("highVel", 127);
            if (! top && z->getIntAttribute ("lowVel") == hi + 1) z->setAttribute ("lowVel", lo);
        }
        xml->writeTo (mapFile);
    }
    // 3) broj slojeva u oznaci
    const auto tag = dir.getChildFile ("kronos-ms.txt");
    if (tag.existsAsFile())
    {
        auto lines = juce::StringArray::fromLines (tag.loadFileAsString());
        for (auto& l : lines) if (l.startsWith ("layers=")) l = "layers=" + juce::String ((int) layers.size() - 1);
        tag.replaceWithText (lines.joinIntoString ("\n"));
    }
    // 4) svi useri koji koriste ovaj instrument: OSC sa razlozenim slojevima se ponovo slozi (pretapanje, dinamika);
    //    inace MS tog sloja se gasi, a susedni MS preuzima opseg
    const auto remaining = velocityLayersOf (os.instrument);
    for (int u = 0; u < (int) users.size(); ++u)
    {
        auto user = users[(size_t) u];
        bool changed = false;
        std::array<bool, kNumBlocks> redone {};
        for (int b = 0; b < kNumBlocks; ++b)
        {
            int mine = 0, other = 0;
            for (int m = 0; m < kMsPerOsc; ++m)
            {
                const auto& o = user.osc[(size_t) (b * kMsPerOsc + m)];
                if (o.on) (o.instrument == os.instrument ? mine : other)++;
            }
            if (mine > 1 && other == 0)
            {
                auto proto = user.osc[(size_t) (b * kMsPerOsc)];
                if (! proto.on || proto.instrument != os.instrument)
                    for (int m = 0; m < kMsPerOsc; ++m)
                        if (user.osc[(size_t) (b * kMsPerOsc + m)].on) { proto = user.osc[(size_t) (b * kMsPerOsc + m)]; break; }
                if (remaining.size() >= 2) layoutVelocityLayers (user, b, proto, remaining);
                else
                {
                    proto.velLow = 1; proto.velHigh = 127; proto.fadeIn = proto.fadeOut = 0; proto.velRampDb = 0.0f;
                    for (int m = 0; m < kMsPerOsc; ++m) { auto& o = user.osc[(size_t) (b * kMsPerOsc + m)]; o = OscSettings(); o.block = b; }
                    user.osc[(size_t) (b * kMsPerOsc)] = proto;
                    user.block[(size_t) b].velSens = 0.6f;
                }
                changed = true;
                redone[(size_t) b] = true;
            }
        }
        for (auto& o : user.osc)
        {
            if (! o.on || o.instrument != os.instrument || redone[(size_t) juce::jlimit (0, kNumBlocks - 1, o.block)]) continue;
            if (o.velLow >= lo && o.velHigh <= hi) { const int b = o.block; o = OscSettings(); o.block = b; changed = true; continue; }
            if (top && o.velHigh == lo - 1) { o.velHigh = 127; changed = true; }
            if (! top && o.velLow == hi + 1) { o.velLow = lo; changed = true; }
        }
        if (changed) updateUser (u, user);
    }
    reloadInstrument (os.instrument, false);
    report = T ("Deleted ") + juce::String (deleted) + T (" files (velocity layer ") + juce::String (layer) + T (", range ")
           + juce::String (lo) + "-" + juce::String (hi) + T ("). ")
           + (top ? T ("The softer layer now goes up to 127.") : T ("The louder layer now goes down from ") + juce::String (lo) + ".");
    return true;
}

void LiveSamplerProcessor::setCurrentBank (int b)
{
    currentBank = juce::jlimit (0, kNumPerfBanks - 1, b);
    markSave();
    sendChangeMessage();
}

juce::String LiveSamplerProcessor::getBankName (int b) const
{
    b = juce::jlimit (0, kNumPerfBanks - 1, b);
    return bankNames[(size_t) b].isNotEmpty() ? bankNames[(size_t) b] : T ("List ") + juce::String (b + 1);
}

void LiveSamplerProcessor::setBankName (int b, const juce::String& n)
{
    bankNames[(size_t) juce::jlimit (0, kNumPerfBanks - 1, b)] = n.trim();
    markSave();
    sendChangeMessage();
}

juce::StringArray LiveSamplerProcessor::defaultCategories()
{
    // iste kao Kronos kategorije u Live Kontroleru
    return { "Keys", "Acc", "Winds", "Sax", "Brass", "Strings", "Pluck", "Guitar", "Synth", "Lead", "Bass", "Pesme", "Main" };
}

std::vector<juce::Colour> LiveSamplerProcessor::defaultCategoryColours()
{
    return { juce::Colour (0xff88a4c5), juce::Colour (0xff6a7f96), juce::Colour (0xffab81a2), juce::Colour (0xffaa842a),
             juce::Colour (0xff808080), juce::Colour (0xffb23f3f), juce::Colour (0xff7f4236), juce::Colour (0xff374520),
             juce::Colour (0xff9267ba), juce::Colour (0xff5360a5), juce::Colour (0xff91a730), juce::Colour (0xfff5e663),
             juce::Colour (0xfff5e663) };
}

juce::File LiveSamplerProcessor::defaultKontrolerFile()
{
    return juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("Live Kontroler").getChildFile ("podesavanja-server.json");
}

namespace
{
    // uredjaj u Live Kontroleru: kljuc ciji je naziv "Pikaso Workstation" (ili kljuc "pikasoworkstation")
    juce::String kontrolerDeviceId (const juce::var& json)
    {
        if (auto* devs = json.getProperty ("devices", {}).getDynamicObject())
            for (auto& p : devs->getProperties())
                if (p.value.getProperty ("name", {}).toString().trim().equalsIgnoreCase ("Pikaso Workstation"))
                    return p.name.toString();
        return "pikasoworkstation";
    }

    juce::String stripDevice (const juce::String& name)
    {
        const auto dot = juce::String::fromUTF8 ("\xc2\xb7");
        return (name.contains (dot) ? name.fromFirstOccurrenceOf (dot, false, false) : name).trim();
    }

    // mesto performansa iz stavke Live Kontrolera: Bank Select MSB (CC0) = lista, Program Change = mesto u listi
    int itemSlot (const juce::var& item, const juce::String& dev)
    {
        if (const auto* actions = item.getProperty ("do", {}).getArray())
            for (auto& a : *actions)
                if (a.getProperty ("device", {}).toString() == dev && a.getProperty ("type", {}).toString() == "pc")
                {
                    const int list = a.hasProperty ("msb") && ! a["msb"].isVoid() ? (int) a["msb"] : 0;
                    const int pc = (int) a.getProperty ("pc", 0);
                    if (list < 0 || list >= kNumPerfBanks || pc < 0 || pc >= kPerfPerBank) return -1;
                    return list * kPerfPerBank + pc;
                }
        return -1;
    }

    juce::String colourHex (juce::Colour c) { return "#" + c.toDisplayString (false).toLowerCase(); }
}

// Salje izmene u Live Kontroler preko most.py (GET /state, izmena samo kategorija uredjaja Pikaso Workstation, PUT /state).
// most.py pravi sigurnosne kopije i javlja iPad-u; ako most nije pokrenut, pokusava ponovo dok ne uspe.
class KontrolerPusher : public juce::Thread
{
public:
    struct Snapshot
    {
        juce::StringArray names;
        std::vector<juce::Colour> colours;
        std::map<int, std::pair<juce::String, juce::String>> slots;   // mesto -> (ime performansa, kategorija)
        std::map<juce::String, juce::String> renames;
    };

    KontrolerPusher (LiveSamplerProcessor& p) : juce::Thread ("Live Kontroler"), proc (p) {}

    void post (Snapshot s)
    {
        const juce::ScopedLock sl (lock);
        pending = std::make_unique<Snapshot> (std::move (s));
        notify();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            std::unique_ptr<Snapshot> snap;
            {
                const juce::ScopedLock sl (lock);
                if (pending != nullptr) snap = std::make_unique<Snapshot> (*pending);
            }
            if (snap == nullptr) { wait (-1); continue; }
            const auto result = push (*snap);
            if (result.isEmpty())
            {
                const juce::ScopedLock sl (lock);
                pending.reset();
            }
            setStatus (result.isEmpty() ? juce::String::fromUTF8 ("Sent to Live Kontroler ") + juce::Time::getCurrentTime().toString (false, true)
                                        : result);
            if (result.isNotEmpty()) wait (5000);   // most nije pokrenut: probaj ponovo
        }
    }

    juce::String status() const { const juce::ScopedLock sl (lock); return statusText; }

private:
    void setStatus (const juce::String& s) { const juce::ScopedLock sl (lock); statusText = s; }

    static juce::String push (const Snapshot& snap)
    {
        const juce::URL base ("http://localhost:8080/state");
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            int code = 0;
            auto in = base.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                  .withConnectionTimeoutMs (2000).withStatusCode (&code));
            if (in == nullptr || code != 200)
                return juce::String::fromUTF8 ("Waiting for most.py (Live Kontroler is not running)");
            const auto text = in->readEntireStreamAsString();
            in.reset();
            auto json = juce::JSON::parse (text);
            // __ts tacno kako ga je most.py upisao (da poredjenje na serveru ne izgubi decimale)
            juce::String baseTs;
            {
                const int at = text.indexOf ("\"__ts\"");
                if (at >= 0)
                {
                    auto rest = text.substring (at + 6).trimStart().trimCharactersAtStart (":").trimStart();
                    baseTs = rest.initialSectionContainingOnly ("0123456789.eE+-");
                }
            }
            auto* root = json.getDynamicObject();
            auto* cats = json.getProperty ("categories", {}).getArray();
            if (root == nullptr || cats == nullptr) return "Live Kontroler: unknown format";
            const auto dev = kontrolerDeviceId (json);
            auto devName = json.getProperty ("devices", {}).getProperty (dev, {}).getProperty ("name", "Pikaso Workstation").toString();

            // postojece kategorije i stavke ovog uredjaja (da ostanu id, ikone i ostalo sto je podeseno u Live Kontroleru)
            int insertAt = -1;
            std::map<juce::String, juce::var> oldCats;
            std::map<int, juce::var> oldItems;
            juce::Array<juce::var> kept;
            for (auto& c : *cats)
            {
                if (c.getProperty ("device", {}).toString() == dev
                    && stripDevice (c.getProperty ("name", {}).toString()).equalsIgnoreCase ("Main"))
                {
                    kept.add (c);   // Main: dugmad koju korisnik kopira u Pikaso Stage, ostaje netaknuta
                }
                else if (c.getProperty ("device", {}).toString() == dev)
                {
                    if (insertAt < 0) insertAt = kept.size();
                    oldCats[stripDevice (c.getProperty ("name", {}).toString()).toLowerCase()] = c;
                    if (auto* items = c.getProperty ("items", {}).getArray())
                        for (auto& it : *items)
                            if (const int sl = itemSlot (it, dev); sl >= 0 && oldItems.count (sl) == 0) oldItems[sl] = it;
                }
                else kept.add (c);
            }
            if (insertAt < 0) insertAt = kept.size();

            juce::Array<juce::var> mine;
            for (int k = 0; k < snap.names.size(); ++k)
            {
                const auto& nm = snap.names[k];
                juce::var cat;
                if (auto it = oldCats.find (nm.toLowerCase()); it != oldCats.end()) cat = it->second;
                else
                    for (auto& [from, to] : snap.renames)
                        if (to.equalsIgnoreCase (nm))
                            if (auto it2 = oldCats.find (from.toLowerCase()); it2 != oldCats.end()) cat = it2->second;
                if (! cat.isObject())
                {
                    cat = juce::var (new juce::DynamicObject());
                    cat.getDynamicObject()->setProperty ("icon", "star");
                }
                auto* o = cat.getDynamicObject();
                o->setProperty ("name", devName + juce::String::fromUTF8 (" \xc2\xb7 ") + nm);
                o->setProperty ("device", dev);
                if (k < (int) snap.colours.size()) o->setProperty ("color", colourHex (snap.colours[(size_t) k]));
                juce::Array<juce::var> items;
                for (auto& [slot, pr] : snap.slots)
                {
                    if (! pr.second.equalsIgnoreCase (nm)) continue;
                    juce::var item;
                    if (auto it = oldItems.find (slot); it != oldItems.end()) item = it->second;
                    if (! item.isObject())
                    {
                        item = juce::var (new juce::DynamicObject());
                        auto* a = new juce::DynamicObject();
                        a->setProperty ("device", dev);
                        a->setProperty ("type", "pc");
                        a->setProperty ("msb", slot / kPerfPerBank);
                        a->setProperty ("pc", slot % kPerfPerBank);
                        juce::Array<juce::var> acts;
                        acts.add (juce::var (a));
                        item.getDynamicObject()->setProperty ("do", acts);
                        item.getDynamicObject()->setProperty ("id", "pw" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt()).substring (0, 7));
                    }
                    item.getDynamicObject()->setProperty ("name", pr.first);
                    items.add (item);
                }
                o->setProperty ("items", items);
                mine.add (cat);
            }
            for (int k = 0; k < mine.size(); ++k) kept.insert (insertAt + k, mine[k]);
            root->setProperty ("categories", kept);

            int putCode = 0;
            auto out = base.withPOSTData (juce::JSON::toString (json, true))
                           .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                                   .withHttpRequestCmd ("PUT")
                                                   .withExtraHeaders ("Content-Type: application/json; charset=utf-8\r\nX-Base-Ts: " + baseTs)
                                                   .withConnectionTimeoutMs (3000).withStatusCode (&putCode));
            if (out != nullptr) out->readEntireStreamAsString();
            if (putCode == 200) return {};
            if (putCode != 409) return "Live Kontroler: send error (" + juce::String (putCode) + ")";
            // 409: iPad je u medjuvremenu snimio nesto; procitaj ponovo i probaj
        }
        return juce::String::fromUTF8 ("Live Kontroler: keeps changing, will try again");
    }

    LiveSamplerProcessor& proc;
    juce::CriticalSection lock;
    std::unique_ptr<Snapshot> pending;
    juce::String statusText;
};

juce::String LiveSamplerProcessor::getKontrolerStatus() const
{
    if (! kontrolerLinked) return T ("Not linked to Live Kontroler");
    if (auto* p = dynamic_cast<KontrolerPusher*> (kontrolerPusher.get()); p != nullptr && p->status().isNotEmpty()) return p->status();
    return kontrolerStatus;
}

void LiveSamplerProcessor::kontrolerChanged()
{
    if (! kontrolerLinked) return;
    KontrolerPusher::Snapshot s;
    s.names = categories;
    s.colours = categoryColours;
    s.renames = kontrolerRenames;
    for (int i = 0; i < kNumPerfSlots; ++i)
    {
        const auto& p = slots[(size_t) i];
        if (p.used && categoryIndex (p) >= 0) s.slots[i] = { p.name, categories[categoryIndex (p)] };
    }
    // ono sto saljemo je ujedno i ono sto ce Live Kontroler imati
    kontrolerLast.clear();
    for (auto& [slot, pr] : s.slots) kontrolerLast[slot] = pr.second;
    kontrolerRenames.clear();
    if (kontrolerPusher == nullptr)
    {
        kontrolerPusher = std::make_unique<KontrolerPusher> (*this);
        kontrolerPusher->startThread();
    }
    static_cast<KontrolerPusher*> (kontrolerPusher.get())->post (std::move (s));
}

void LiveSamplerProcessor::linkKontroler (const juce::File& f)
{
    kontrolerLinked = f != juce::File();
    if (kontrolerLinked) kontrolerFile = f;
    kontrolerTime = {};
    kontrolerLast.clear();
    if (kontrolerLinked) syncKontroler();
    markSave();
    sendChangeMessage();
}

// Live Kontroler -> ovde: kategorije uredjaja "Pikaso Workstation" (ime, boja, redosled) i koji performans je u kojoj.
// Izmena u Live Kontroleru (stavka premestena / obrisana) ima prednost; ono sto je promenjeno ovde salje se nazad.
bool LiveSamplerProcessor::syncKontroler (juce::String* report)
{
    if (! kontrolerFile.existsAsFile())
    {
        if (report != nullptr) *report = T ("Cannot find ") + kontrolerFile.getFullPathName();
        return false;
    }
    kontrolerTime = kontrolerFile.getLastModificationTime();
    const auto json = juce::JSON::parse (kontrolerFile.loadFileAsString());
    const auto* cats = json.getProperty ("categories", {}).getArray();
    if (cats == nullptr)
    {
        if (report != nullptr) *report = T ("The file has no categories.");
        return false;
    }
    const auto dev = kontrolerDeviceId (json);
    loadImpulseLayout (json);

    juce::StringArray names;
    std::vector<juce::Colour> colours;
    std::map<int, juce::String> remote;
    for (auto& c : *cats)
    {
        if (c.getProperty ("device", {}).toString() != dev) continue;
        const auto name = stripDevice (c.getProperty ("name", {}).toString());
        if (name.isEmpty() || names.contains (name, true)) continue;
        if (name.equalsIgnoreCase ("Main")) continue;   // Main = kopije dugmadi koje slaze korisnik u Pikaso Stage, nije kategorija performansa
        names.add (name);
        const auto col = c.getProperty ("color", {}).toString().trimCharactersAtStart ("#");
        colours.push_back (col.length() == 6 ? juce::Colour ((juce::uint32) (0xff000000u | (juce::uint32) col.getHexValue32()))
                                             : juce::Colour (0xff8a93a6));
        if (auto* items = c.getProperty ("items", {}).getArray())
            for (auto& it : *items)
                if (const int sl = itemSlot (it, dev); sl >= 0 && remote.count (sl) == 0) remote[sl] = name;
    }
    if (names.isEmpty())
    {
        if (report != nullptr) *report = T ("Live Kontroler has no categories for the device Pikaso Workstation.");
        kontrolerStatus = T ("Live Kontroler: no device Pikaso Workstation");
        return false;
    }

    categories = names;
    categoryColours = colours;
    int fromRemote = 0, localOnly = 0;
    bool needPush = false;
    for (int i = 0; i < kNumPerfSlots; ++i)
    {
        auto& p = slots[(size_t) i];
        const auto r = remote.count (i) ? remote[i] : juce::String();
        const auto last = kontrolerLast.count (i) ? kontrolerLast[i] : juce::String();
        if (! p.used) { if (r.isNotEmpty()) needPush = true; continue; }
        if (r != last && (r.isNotEmpty() || last.isNotEmpty()))
        {
            p.category = r;           // promenjeno u Live Kontroleru
            ++fromRemote;
        }
        const auto local = categoryIndex (p) >= 0 ? p.category : juce::String();
        if (! local.equalsIgnoreCase (r)) { needPush = true; ++localOnly; }
    }
    kontrolerLast = remote;
    if (currentSlot >= 0) perf.category = slots[(size_t) currentSlot].category;
    kontrolerStatus = T ("Live Kontroler read ") + juce::Time::getCurrentTime().toString (false, true);
    markSave();
    sendChangeMessage();
    if (needPush) kontrolerChanged();
    if (report != nullptr)
        *report = juce::String (names.size()) + T (" categories (device Pikaso Workstation), ") + juce::String ((int) remote.size())
                + T (" performances in them.\n") + juce::String (fromRemote) + T (" changed from Live Kontroler, ")
                + juce::String (localOnly) + T (" sent from here.");
    return true;
}

void LiveSamplerProcessor::setCategories (const juce::StringArray& names, const std::vector<juce::Colour>& colours, const std::vector<int>& remap)
{
    auto fix = [&remap] (int c) { return c >= 0 && c < (int) remap.size() ? remap[(size_t) c] : -1; };
    auto rename = [&] (juce::String& cat)
    {
        const int nw = fix (categories.indexOf (cat, true));
        if (cat.isNotEmpty() && categories.contains (cat, true)) cat = nw >= 0 ? names[nw] : juce::String();
    };
    for (auto& s : slots) if (s.used) rename (s.category);
    rename (perf.category);
    // kombiji vezani za obrisanu kategoriju gube vezu; preimenovana nosi novo ime
    for (int k = 0; k < categories.size(); ++k)
        if (const int nw = fix (k); nw >= 0 && names[nw] != categories[k]) kontrolerRenames[categories[k]] = names[nw];
    categories = names;
    categoryColours = colours;
    categoryColours.resize ((size_t) categories.size(), juce::Colour (0xff8a93a6));
    categoriesEdited = true;
    markSave();
    sendChangeMessage();
    kontrolerChanged();
}

void LiveSamplerProcessor::setSlotCategory (int slot, int category)
{
    if (slot < 0 || slot >= kNumPerfSlots) return;
    slots[(size_t) slot].category = categoryName (category);
    if (slot == currentSlot) perf.category = slots[(size_t) slot].category;
    markSave();
    sendChangeMessage();
    kontrolerChanged();
}

void LiveSamplerProcessor::clearPerformanceSlot (int slot)
{
    if (slot < 0 || slot >= kNumPerfSlots) return;
    slots[(size_t) slot] = Performance();
    if (currentSlot == slot) currentSlot = -1;
    markSave();
    sendChangeMessage();
    kontrolerChanged();
}

void LiveSamplerProcessor::swapPerformanceSlots (int a, int b)
{
    if (a < 0 || b < 0 || a >= kNumPerfSlots || b >= kNumPerfSlots || a == b) return;
    std::swap (slots[(size_t) a], slots[(size_t) b]);
    if (currentSlot == a) currentSlot = b;
    else if (currentSlot == b) currentSlot = a;
    // dugmad set lista pokazuju na isti performans i posle premestanja
    for (auto& sl : setLists)
        for (auto& s : sl.slots)
            if (s.perf == a) s.perf = b;
            else if (s.perf == b) s.perf = a;
    markSave();
    sendChangeMessage();
    kontrolerChanged();
}

void LiveSamplerProcessor::updateSetList (int i, const SetList& s)
{
    setLists[(size_t) juce::jlimit (0, kNumSetLists - 1, i)] = s;
    markSave();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::setCurrentSetList (int i)
{
    currentSetList = juce::jlimit (0, kNumSetLists - 1, i);
    currentSetListSlot = -1;
    markSave();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::setSetListPage (int p)
{
    setListPage = juce::jlimit (0, kSetListSlots / 16 - 1, p);
    markSave();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::recallSetListSlot (int slot)
{
    if (slot < 0 || slot >= kSetListSlots) return;
    const int p = setLists[(size_t) currentSetList].slots[(size_t) slot].perf;
    if (p < 0 || p >= kNumPerfSlots || ! slots[(size_t) p].used) return;
    currentSetListSlot = slot;
    if (slot / 16 != setListPage) setListPage = slot / 16;
    recallPerformance (p);
}

//==============================================================================
//  Terce (message nit)
//==============================================================================
void LiveSamplerProcessor::updateTerca (const TercaSettings& t)
{
    terca = t;
    if (tercaScale.load() >= (int) terca.scales.size())
        tercaScale = 0;
    markSave();
    rebuildConfig();
    sendChangeMessage();
}

void LiveSamplerProcessor::setTercaOn (bool on)
{
    tercaOn = on;
    sendChangeMessage();
}

void LiveSamplerProcessor::setTercaKey (int root, int scale)
{
    tercaRoot = ((root % 12) + 12) % 12;
    tercaScale = juce::jlimit (0, juce::jmax (0, (int) terca.scales.size() - 1), scale);
    sendChangeMessage();
}

void LiveSamplerProcessor::tercaButton (bool down)
{
    int start1, size1, start2, size2;
    uiTriggerFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
    {
        uiTriggerData[start1] = down ? 1 : 0;
        uiTriggerFifo.finishedWrite (1);
    }
}

juce::String LiveSamplerProcessor::getTercaText() const
{
    return Terca::keyName (tercaRoot.load(), terca, tercaScale.load());
}

//==============================================================================
//  Audio
//==============================================================================
void LiveSamplerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    maxBlock = juce::jmax (64, samplesPerBlock);
    for (auto& bank : partBanks)
        for (auto& p : bank)
            p.prepare (sampleRate, maxBlock);
    master.prepare (sampleRate, maxBlock);
    mainBuf.setSize (2, maxBlock);
    reverbBuf.setSize (2, maxBlock);
    delayBuf.setSize (2, maxBlock);
    std::memset (routed, 0, sizeof (routed));
    std::memset (harmonized, 0, sizeof (harmonized));
    std::memset (heldCount, 0, sizeof (heldCount));
    keyboardState.reset();
}

int LiveSamplerProcessor::heldMask() const
{
    int m = 0;
    for (int n = 0; n < 128; ++n)
        if (heldCount[n] > 0)
            m |= 1 << (n % 12);
    return m;
}

void LiveSamplerProcessor::tryRecognize (int mask, const EngineConfig& c)
{
    int root, scale;
    if (Terca::recognize (c.terca, mask, root, scale))
    {
        tercaRoot = root;
        tercaScale = scale;
        tercaOn = true;
        recognizedInCapture = true;
        tercaChanged = true;
        triggerAsyncUpdate();
    }
}

void LiveSamplerProcessor::tercaTrigger (bool down, const EngineConfig& c)
{
    if (down)
    {
        if (tercaCapturing)
            return;
        tercaCapturing = true;
        recognizedInCapture = false;
        capturedMask = 0;
        if (const int held = heldMask(); held != 0)
            tryRecognize (held, c);
    }
    else
    {
        if (! tercaCapturing)
            return;
        tercaCapturing = false;
        if (! recognizedInCapture)
            tercaOn = ! tercaOn.load();   // kratak pritisak: pali / gasi
        tercaChanged = true;
        triggerAsyncUpdate();
    }
}

void LiveSamplerProcessor::routeNoteOn (int ch, int note, int vel, const EngineConfig& c)
{
    ++heldCount[note];

    if (tercaCapturing)
    {
        // dugme za tercu je drzano: akord se samo uci (dur / mol / tonalitet), ne cuje se
        capturedMask |= 1 << (note % 12);
        tryRecognize (capturedMask | heldMask(), c);
        return;
    }

    const bool tOn = tercaOn.load();
    const int root = tercaRoot.load(), scale = tercaScale.load();

    juce::uint16 used = 0, harm = 0;
    for (int p = 0; p < kNumParts; ++p)
    {
        const auto& pr = c.parts[(size_t) p];
        const auto& part = pr.part;
        if (! pr.active)
            continue;
        if (part.midiChannel != 0 && part.midiChannel != ch + 1)
            continue;
        if (note < part.keyLow || note > part.keyHigh || vel < part.velLow || vel > part.velHigh)
            continue;

        const int shift = part.transpose + part.octave * 12 + c.globalTranspose;
        parts[p].noteOn (note, note + shift, vel);
        used |= (juce::uint16) (1 << p);

        if (pr.user.isMono())
        {
            // mono: terca prati notu koja svira (legato), vidi monoHarmUpdate
            monoHarm[(size_t) p].add (note, vel);
            monoHarmUpdate (p, c);
        }
        else if (tOn && part.terca && ! tercaCapturing)
        {
            // terca samo na najvisi drzani ton u opsegu ovog dela (melodija)
            bool isTop = true;
            if (c.terca.topNoteOnly)
                for (int n = note + 1; n <= part.keyHigh && n < 128; ++n)
                    if (heldCount[n] > 0) { isTop = false; break; }

            if (isTop)
            {
                // nova melodijska nota: nize note ovog dela gube tercu
                if (c.terca.topNoteOnly)
                    for (int cc = 0; cc < 16; ++cc)
                        for (int n = part.keyLow; n < note && n < 128; ++n)
                            if (harmonized[cc][n] & (1 << p))
                            {
                                parts[p].noteOff (128 + n);
                                harmonized[cc][n] &= (juce::uint16) ~(1 << p);
                            }

                int hn[2];
                const int count = Terca::harmonize (c.terca, root, scale, note, hn);
                const int hv = juce::jlimit (1, 127, vel);   // jacina terce ide preko harmGain (glasnoca), ne preko velocity
                for (int k = 0; k < count; ++k)
                    parts[p].noteOn (128 + note, hn[k] + shift, hv, k > 0);
                if (count > 0)
                    harm |= (juce::uint16) (1 << p);
            }
        }
    }
    routed[ch][note] |= used;
    harmonized[ch][note] |= harm;
}

void LiveSamplerProcessor::monoHarmUpdate (int p, const EngineConfig& c)
{
    auto& H = monoHarm[(size_t) p];
    const auto& pr = c.parts[(size_t) p];
    const bool want = tercaOn.load() && pr.active && pr.part.terca && pr.user.isMono() && ! tercaCapturing;
    const int root = tercaRoot.load(), scale = tercaScale.load();

    // melodija = ton koji mono deo svira (isti prioritet kao u useru)
    int mel = -1, vel = 0;
    if (want && H.n > 0)
    {
        int k = H.n - 1;
        if (pr.user.priority == Priority::High) { for (int i = 0; i < H.n; ++i) if (H.note[i] > H.note[k]) k = i; }
        else if (pr.user.priority == Priority::Low) { for (int i = 0; i < H.n; ++i) if (H.note[i] < H.note[k]) k = i; }
        mel = H.note[k]; vel = H.vel[k];
    }
    if (mel == H.curMel && root == H.root && scale == H.scale && want == H.on) return;
    H.root = root; H.scale = scale; H.on = want;

    int hn[2] {}, count = 0;
    if (mel >= 0) count = Terca::harmonize (c.terca, root, scale, mel, hn);
    if (mel < 0 || count <= 0)
    {
        if (H.curId >= 0) parts[p].noteOff (H.curId);
        H.curId = -1; H.curMel = mel;
        return;
    }
    // novi id dok je stari jos drzan = mono legato prelaz (bez novog napada); stari se odmah pusta
    const int shift = pr.part.transpose + pr.part.octave * 12 + c.globalTranspose;
    const int hv = juce::jlimit (1, 127, vel);   // jacina terce ide preko harmGain (glasnoca), ne preko velocity
    const int newId = H.curId == 254 ? 255 : 254;
    for (int k = 0; k < count; ++k)
        parts[p].noteOn (newId, hn[k] + shift, hv, k > 0);
    if (H.curId >= 0) parts[p].noteOff (H.curId);
    H.curId = newId; H.curMel = mel;
}

void LiveSamplerProcessor::routeNoteOff (int ch, int note)
{
    if (heldCount[note] > 0)
        --heldCount[note];

    for (int p = 0; p < kNumParts; ++p)
    {
        if (routed[ch][note] & (1 << p))
        {
            parts[p].noteOff (note);
            if (audioConfig != nullptr && audioConfig->parts[(size_t) p].user.isMono())
            {
                monoHarm[(size_t) p].remove (note);
                monoHarmUpdate (p, *audioConfig);   // melodija se vratila na drzani taster ili je pustena: terca prati
            }
        }
        if (harmonized[ch][note] & (1 << p))
            parts[p].noteOff (128 + note);
        if (tailConfig != nullptr)
        {
            tails[p].noteOff (note);        // ton odsviran u prethodnom performansu
            tails[p].noteOff (128 + note);
        }
        auto& H = monoHarm[(size_t) p];
        if (H.tailId >= 0)
        {
            for (int i = 0; i < H.tailN; ++i)
                if (H.tailNote[i] == note) { for (int k = i; k + 1 < H.tailN; ++k) H.tailNote[k] = H.tailNote[k + 1]; --H.tailN; break; }
            if (H.tailN <= 0) { tails[p].noteOff (H.tailId); H.tailId = -1; }
        }
    }
    routed[ch][note] = 0;
    harmonized[ch][note] = 0;
}

void LiveSamplerProcessor::handleMidi (const juce::MidiMessage& m, const EngineConfig& c)
{
    const int ch = juce::jlimit (0, 15, m.getChannel() - 1);
    const auto& lt = c.terca.learn;

    // Pikaso Stage (SysEx, ID 7D = sopstveni uredjaj): F0 7D 50 01 kk F7 tonalitet harmonije (0-11),
    // F0 7D 50 02 mm F7 harmonija: 0 iskljucena, 1 dur (Major), 2 mol (Minor)
    if (m.isSysEx())
    {
        const auto* d = m.getSysExData();
        if (m.getSysExDataSize() >= 4 && d[0] == 0x7D && d[1] == 0x50)
        {
            if (d[2] == 0x03)
            {
                // master transpozicija iz Pikaso Stage: ceo Workstation, svi performansi (0x40 = 0)
                const int st = juce::jlimit (-24, 24, (int) d[3] - 0x40);
                juce::MessageManager::callAsync ([this, st, a = alive] { if (a->load()) setGlobalTranspose (st); });
                return;
            }
            if (d[2] == 0x01) tercaRoot = d[3] % 12;
            else if (d[2] == 0x02)
            {
                if (d[3] == 0) tercaOn = false;
                else
                {
                    const char* want = d[3] == 2 ? "Minor" : "Major";
                    for (int i = 0; i < (int) c.terca.scales.size(); ++i)
                        if (c.terca.scales[(size_t) i].name.equalsIgnoreCase (want)) { tercaScale = i; break; }
                    tercaOn = true;
                }
            }
            tercaChanged = true;
            triggerAsyncUpdate();
        }
        return;
    }

    // IMPULSE pickup: posle pomeranja sa iPada pravi knob / fader se ne cuje dok ne stigne do te vrednosti
    if (m.isController() && ! impInjecting && m.getChannel() == impCh.load())
    {
        const int num = m.getControllerNumber(), v = m.getControllerValue();
        const int want = impPickup[(size_t) num].load();
        if (want >= 0)
        {
            const int prev = impPhysPrev[(size_t) num];
            impPhysPrev[(size_t) num] = v;
            const bool crossed = std::abs (v - want) <= 2 || (prev >= 0 && ((prev < want) != (v < want)));
            if (! crossed) return;
            impPickup[(size_t) num] = -1;
        }
        else impPhysPrev[(size_t) num] = v;
    }

    // MIDI Learn (desni klik na kontrolu): naucen CC / taster ide samo na tu kontrolu
    if (MidiLearn::get().handleIncoming (m))
        return;

    // dodela dugmeta za terce
    if (learning.load())
    {
        if (m.isController() && m.getControllerValue() > 0 && m.getControllerNumber() != 64)
        {
            learnedChannel = 0;
            learnedNumber = m.getControllerNumber();
            learnedType = LearnTrigger::CC;
            learning = false;
            triggerAsyncUpdate();
            return;
        }
        if (m.isNoteOn())
        {
            learnedChannel = m.getChannel();
            learnedNumber = m.getNoteNumber();
            learnedType = LearnTrigger::Note;
            learning = false;
            triggerAsyncUpdate();
            return;
        }
    }

    if (m.isController() && lt.matchesCC (m.getChannel(), m.getControllerNumber()))
    {
        if (isImpulseLatching (m.getChannel(), m.getControllerNumber()))
        {
            // Impulse dugme u toggle modu: svaki pritisak (127 ili 0) pali / gasi tercu
            tercaTrigger (true, c);
            tercaTrigger (false, c);
        }
        else tercaTrigger (m.getControllerValue() >= 64, c);
        return;
    }
    if ((m.isNoteOn() || m.isNoteOff()) && lt.matchesNote (m.getChannel(), m.getNoteNumber()))
    {
        tercaTrigger (m.isNoteOn(), c);
        return;
    }

    if (m.isNoteOn() && keyLearnMode.load() != 0)
    {
        // opseg / split sviranjem: taster se ne cuje, samo se zapamti
        keyLearnNote = m.getNoteNumber();
        return;
    }
    if (m.isNoteOn() && qtEdit.load())
    {
        // QT EDIT (kao Korg Pa / Ketron): pritisnut taster menja ton izmedju 0 i -50 centi, ne svira
        qtMask.fetch_or (1 << (m.getNoteNumber() % 12));
        return;
    }
    if (m.isNoteOn())
    {
        routeNoteOn (ch, m.getNoteNumber(), shapeVelocity (m.getVelocity()), c);
    }
    else if (m.isNoteOff())
    {
        routeNoteOff (ch, m.getNoteNumber());
    }
    else if (m.isSustainPedalOn() || m.isSustainPedalOff())
    {
        sustainHeld = m.isSustainPedalOn();
        for (auto& p : parts)
            p.setSustain (sustainHeld);
        for (auto& p : tails)
            p.setSustain (sustainHeld);
    }
    else if (m.isController() && learnVector.load() != 0)
    {
        // LEARN: prvi CC koji stigne postaje X ili Y vector joysticka
        (learnVector.load() == 1 ? vectorCcX : vectorCcY) = m.getControllerNumber();
        learnVector = 0;
        triggerAsyncUpdate();
    }
    else if (m.isController() && (m.getControllerNumber() == vectorCcX.load() || m.getControllerNumber() == vectorCcY.load()))
    {
        const float v = juce::jlimit (-1.0f, 1.0f, ((float) m.getControllerValue() - 64.0f) / (m.getControllerValue() >= 64 ? 63.0f : 64.0f));
        (m.getControllerNumber() == vectorCcX.load() ? vectorX : vectorY) = v;
        vectorUiDirty = true;
    }
    else if (m.isControllerOfType (11))      // Expression: samo delovi kojima je ukljucen EXPR
    {
        const float v = (float) m.getControllerValue() / 127.0f;
        for (auto& p : parts)
            p.setExpression (v);
    }
    else if (m.isControllerOfType (1))
    {
        const float v = (float) m.getControllerValue() / 127.0f;
        for (auto& p : parts)
            p.setModWheel (v);
    }
    else if (m.isChannelPressure())
    {
        const float v = (float) m.getChannelPressureValue() / 127.0f;
        for (auto& p : parts)
            p.setAftertouch (v);
    }
    else if (m.isPitchWheel())
    {
        const float v = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
        for (auto& p : parts)
            p.setPitchBend (v);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& p : parts)
            p.allNotesOff (m.isAllSoundOff());
        for (auto& p : tails)
            p.allNotesOff (m.isAllSoundOff());
        for (auto& H : monoHarm) { H.reset(); H.tailId = -1; H.tailN = 0; }
        std::memset (routed, 0, sizeof (routed));
        std::memset (harmonized, 0, sizeof (harmonized));
        std::memset (heldCount, 0, sizeof (heldCount));
    }
    else if (m.isController() && m.getControllerNumber() == 0)   // Bank Select MSB: lista / set lista
    {
        pendingBankMsb = juce::jlimit (0, kNumPerfBanks - 1, m.getControllerValue());
        triggerAsyncUpdate();
    }
    else if (m.isProgramChange())
    {
        pendingProgram = m.getProgramChangeNumber();
        triggerAsyncUpdate();
    }
}

void LiveSamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    const auto t0 = juce::Time::getHighResolutionTicks();
    struct LoadMeter
    {
        LiveSamplerProcessor& p; juce::int64 t0; int n;
        ~LoadMeter()
        {
            const double sr = p.getSampleRate();
            if (sr <= 0 || n <= 0) return;
            const double used = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            const float load = (float) (used / (n / sr));
            if (load > 1.0f) p.dropouts.fetch_add (1);   // blok nije stigao na vreme = stvarno pucketanje
            const float prev = p.audioLoad.load();
            p.audioLoad = load > prev ? load : prev * 0.995f + load * 0.005f;
        }
    } loadMeter { *this, t0, n };

    {
        const juce::SpinLock::ScopedLockType sl (configLock);
        if (audioConfig != pendingConfig)
        {
            // drugi performans: trenutni delovi postaju "repovi" (dok drzis tastere / sustain zvuk ostaje), novi delovi primaju note
            if (audioConfig != nullptr && pendingConfig != nullptr && pendingConfig->perfSerial != audioConfig->perfSerial)
            {
                for (auto& p : tails) p.allNotesOff (true);
                std::swap (parts.d, tails.d);
                tailConfig = audioConfig;
                for (auto& p : parts) { p.allNotesOff (true); p.setSustain (sustainHeld); }
                for (auto& H : monoHarm)
                {
                    // terca mono dela ostaje u repu dok se ne puste njeni tasteri
                    H.tailId = H.curId;
                    H.tailN = H.n;
                    for (int i = 0; i < H.n; ++i) H.tailNote[i] = H.note[i];
                    H.reset();
                }
            }
            audioConfig = pendingConfig;
        }
    }
    if (audioConfig == nullptr)
        return;
    const auto& c = *audioConfig;

    keyboardState.processNextMidiBuffer (midi, 0, n, true);

    if (panicRequested.exchange (false))
    {
        for (auto& p : parts)
            p.allNotesOff (true);
        for (auto& p : tails)
            p.allNotesOff (true);
        for (auto& H : monoHarm) { H.reset(); H.tailId = -1; H.tailN = 0; }
        std::memset (routed, 0, sizeof (routed));
        std::memset (harmonized, 0, sizeof (harmonized));
        std::memset (heldCount, 0, sizeof (heldCount));
    }

    // TERCA dugme na ekranu
    {
        int start1, size1, start2, size2;
        const int ready = uiTriggerFifo.getNumReady();
        uiTriggerFifo.prepareToRead (ready, start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i) tercaTrigger (uiTriggerData[start1 + i] != 0, c);
        for (int i = 0; i < size2; ++i) tercaTrigger (uiTriggerData[start2 + i] != 0, c);
        uiTriggerFifo.finishedRead (size1 + size2);
    }

    for (int start = 0; start < n; start += maxBlock)
    {
        const int len = juce::jmin (maxBlock, n - start);

        for (int p = 0; p < kNumParts; ++p)
        {
            parts[p].setVector (vectorX.load(), vectorY.load());
            parts[p].beginBlock (&c.parts[(size_t) p], len);
        }

        if (start == 0)
            for (int p = 0; p < kNumParts; ++p)
                if (monoHarm[(size_t) p].n > 0 || monoHarm[(size_t) p].curId >= 0) monoHarmUpdate (p, c);

        if (start == 0 && impFifo.getNumReady() > 0)
        {
            // komande sa iPada (IMPULSE strana): kao da su stigle sa Impulse-a
            int s1, n1, s2, n2;
            impFifo.prepareToRead (impFifo.getNumReady(), s1, n1, s2, n2);
            impInjecting = true;
            auto run = [&] (int st, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    const auto w = impFifoData[(size_t) (st + i)];
                    const juce::uint8 b[3] { (juce::uint8) (w & 0xff), (juce::uint8) ((w >> 8) & 0x7f), (juce::uint8) ((w >> 16) & 0x7f) };
                    handleMidi (juce::MidiMessage (b, 3, 0.0), c);
                }
            };
            run (s1, n1); run (s2, n2);
            impInjecting = false;
            impFifo.finishedRead (n1 + n2);
        }

        int pos = 0;
        for (const auto meta : midi)
        {
            const int t = meta.samplePosition - start;
            if (t < 0) continue;
            if (t >= len) break;
            if (t > pos)
            {
                for (auto& p : parts) p.renderVoices (pos, t - pos);
                pos = t;
            }
            handleMidi (meta.getMessage(), c);
        }
        for (auto& p : parts)
            p.renderVoices (pos, len - pos);

        mainBuf.clear (0, len);
        reverbBuf.clear (0, len);
        delayBuf.clear (0, len);
        for (auto& p : parts)
            p.endBlock (mainBuf, reverbBuf, delayBuf, len);

        // repovi prethodnog performansa (svaki sa svojim podesavanjima: jacina, EQ, efekat)
        if (tailConfig != nullptr)
        {
            int alive = 0;
            for (int p = 0; p < kNumParts; ++p)
            {
                auto& t = tails[p];
                if (t.getNumActiveVoices() == 0) continue;
                ++alive;
                t.beginBlock (&tailConfig->parts[(size_t) p], len);
                t.renderVoices (0, len);
                t.endBlock (mainBuf, reverbBuf, delayBuf, len);
            }
            if (alive == 0) tailConfig.reset();   // nije poslednja kopija (cuva je configGraveyard), oslobadja se na message niti
        }

        auto ms = c.master;
        ms.masterDb += c.globalDb;
        master.process (mainBuf, reverbBuf, delayBuf, len, ms, c.globalEq);

        if (buffer.getNumChannels() >= 2)
        {
            buffer.copyFrom (0, start, mainBuf, 0, 0, len);
            buffer.copyFrom (1, start, mainBuf, 1, 0, len);
        }
        else if (buffer.getNumChannels() == 1)
        {
            buffer.copyFrom (0, start, mainBuf, 0, 0, len);
            buffer.addFrom (0, start, mainBuf, 1, 0, len);
            buffer.applyGain (0, start, len, 0.5f);
        }
    }

    // presluskivanje sempla iz MAPPING-a
    {
        const juce::SpinLock::ScopedTryLockType tl (previewLock);
        if (tl.isLocked() && previewSmp != nullptr && previewPos >= 0.0 && buffer.getNumChannels() > 0)
        {
            const int len = previewSmp->residentFrames();
            const double inc = previewSmp->sampleRate / juce::jmax (1.0, getSampleRate());
            const float k = previewSmp->pcmScale;
            const juce::int16* L = previewSmp->pcmL != nullptr ? previewSmp->pcmL->data() : nullptr;
            const juce::int16* R = previewSmp->pcmR != nullptr ? previewSmp->pcmR->data() : L;
            if (L == nullptr) previewPos = -1.0;
            auto* o0 = buffer.getWritePointer (0);
            auto* o1 = buffer.getWritePointer (buffer.getNumChannels() > 1 ? 1 : 0);
            for (int i = 0; i < n && previewPos >= 0.0; ++i)
            {
                if (previewPos >= (double) (len - 1)) { previewPos = -1.0; break; }   // kraj: vise se ne svira
                const int ip = juce::jmax (0, (int) previewPos);
                const float t = (float) (previewPos - ip);
                const float g = 0.8f * juce::Decibels::decibelsToGain (globalDb);
                o0[i] += ((float) L[ip] + (float) (L[ip + 1] - L[ip]) * t) * k * g;
                if (o1 != o0) o1[i] += ((float) R[ip] + (float) (R[ip + 1] - R[ip]) * t) * k * g;
                previewPos += inc;
            }
        }
    }

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);

    int voicesNow = 0;
    for (auto& p : parts)
        voicesNow += p.getNumActiveVoices();
    for (auto& p : tails)
        voicesNow += p.getNumActiveVoices();
    activeVoices = voicesNow;
}

void LiveSamplerProcessor::previewSample (SamplePtr s)
{
    SamplePtr old;
    {
        const juce::SpinLock::ScopedLockType sl (previewLock);
        old = previewSmp;
        previewSmp = s;
        previewPos = s != nullptr ? (double) s->startOffset : 0.0;
    }
}

void LiveSamplerProcessor::stopPreview() { previewSample (nullptr); }

void LiveSamplerProcessor::panic()
{
    panicRequested = true;
}

juce::String LiveSamplerProcessor::getStatusText() const
{
    if (keyLearnMode.load() == 2)
        return T ("SPLIT: press the key where part ") + juce::String (keyLearnPart.load() + 1) + T (" starts");
    if (keyLearnMode.load() == 1)
        return keyLearnFirst.load() < 0 ? T ("RANGE: press the LOWEST key of part ") + juce::String (keyLearnPart.load() + 1)
                                        : T ("RANGE: now press the HIGHEST key of part ") + juce::String (keyLearnPart.load() + 1);
    if (qtEdit.load())
        return T ("QT EDIT: keys switch notes between 0 and -50 cents");
    if (libraryFolders.isEmpty())
        return T ("Add a sample folder (SETTINGS)");
    if (scanning)
        return T ("Scanning folders...");
    if (! pendingLoads.empty())
        return T ("Loading: ") + pendingLoads.begin()->first.fromLastOccurrenceOf ("/", false, false)
             + (pendingLoads.size() > 1 ? " (+" + juce::String ((int) pendingLoads.size() - 1) + ")" : juce::String());

    size_t bytes = 0;
    for (auto& kv : cache)
        bytes += kv.second->memoryBytes();

    auto s = juce::String (instrumentList.size()) + T (" instruments  |  ") + juce::String ((double) bytes / (1024.0 * 1024.0), 0) + " MB";
    if (lastWarning.isNotEmpty())
        s << T ("  |  ⚠ ") << lastWarning;
    return s;
}

//==============================================================================
void LiveSamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto xml = perf.toXml();
    xml->setAttribute ("slot", currentSlot);
    copyXmlToBinary (*xml, destData);
}

void LiveSamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("Performance"))
        return;
    perf = Performance::fromXml (*xml);
    currentSlot = xml->getIntAttribute ("slot", -1);
    rebuildConfig();
    loadNeededInstruments();
    sendChangeMessage();
}

juce::AudioProcessorEditor* LiveSamplerProcessor::createEditor()
{
    return new LiveSamplerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LiveSamplerProcessor();
}


//==============================================================================
//  BACKUP / VRATI BACKUP
//==============================================================================
namespace
{
    const char* kBackupLib = "Pikaso Biblioteka";
    const char* kBackupData = "Pikaso podaci.xml";

    // "Kontakt Library/Gudaci/Violina" -> isti put, ali svaki deo kao ispravno ime foldera
    juce::String legalPath (const juce::String& name)
    {
        juce::StringArray parts;
        parts.addTokens (name.replaceCharacter ('\\', '/'), "/", "");
        parts.removeEmptyStrings();
        for (auto& p : parts) p = juce::File::createLegalFileName (p.trim());
        return parts.joinIntoString ("/");
    }
}

class BackupThread : public juce::Thread
{
public:
    BackupThread (LiveSamplerProcessor& p, juce::File d, std::vector<UserProgram> u, juce::File dataFile)
        : juce::Thread ("backup"), proc (p), dest (std::move (d)), users (std::move (u)), data (std::move (dataFile)) {}

    void run() override
    {
        const auto lib = dest.getChildFile (kBackupLib);
        lib.createDirectory();
        juce::StringArray names;
        for (auto& u : users)
            for (auto& o : u.osc)
                if (o.on && o.instrument.isNotEmpty()) names.addIfNotAlreadyThere (o.instrument);

        int done = 0, failed = 0;
        juce::StringArray problems;
        for (auto& name : names)
        {
            if (threadShouldExit()) { setStatus (T ("Backup stopped.")); return; }
            setStatus (T ("Backup: samples ") + juce::String (done + 1) + "/" + juce::String (names.size()) + "  " + name);
            if (! exportInstrument (name, lib.getChildFile (legalPath (name))))
            {
                ++failed;
                problems.add (name);
            }
            ++done;
        }

        // podaci: imena instrumenata u userima pokazuju na kopije u "Pikaso Biblioteka"
        if (auto xml = juce::XmlDocument::parse (data))
        {
            if (auto* us = xml->getChildByName ("Users"))
                for (auto* u : us->getChildWithTagNameIterator ("User"))
                    for (auto* o : u->getChildIterator())
                        if (o->hasAttribute ("instrument") && o->getStringAttribute ("instrument").isNotEmpty())
                            o->setAttribute ("instrument", juce::String (kBackupLib) + "/" + legalPath (o->getStringAttribute ("instrument")));
            if (auto* f = xml->getChildByName ("Folders")) f->deleteAllChildElements();
            xml->setAttribute ("kronosPcg", juce::String());
            xml->writeTo (dest.getChildFile (kBackupData));
        }
        dest.getChildFile ("KAKO VRATITI.txt").replaceWithText (
            "Pikaso Workstation backup (" + juce::Time::getCurrentTime().toString (true, true) + ")\r\n\r\n"
            "On another computer: Pikaso Workstation > SETTINGS > RESTORE BACKUP and choose this folder.\r\n"
            "The folder can stay where it is (USB disk, other disk...): samples are read from '" + juce::String (kBackupLib) + "'.\r\n");
        data.deleteFile();
        setStatus (T ("Backup done: ") + juce::String (names.size() - failed) + T (" instruments in ") + dest.getFullPathName()
                   + (failed > 0 ? T ("  (failed: ") + problems.joinIntoString (", ") + ")" : juce::String()));
    }

    juce::String status() const { const juce::ScopedLock sl (lock); return text; }

private:
    void setStatus (const juce::String& t) { const juce::ScopedLock sl (lock); text = t; }

    // instrument (WAV, Korg, Kontakt...) se upisuje kao WAV folder sa mapom (zone, loop, start/end, jacina)
    bool exportInstrument (const juce::String& name, const juce::File& folder)
    {
        auto inst = proc.library.loadInstrument (name, proc.getC3Is60(), false);
        if (inst == nullptr || inst->samples.empty()) return false;
        folder.createDirectory();
        juce::WavAudioFormat wav;
        Instrument out;
        out.name = name;
        out.type = SourceType::WavFolder;
        out.source = folder;
        out.zones = inst->zones;
        juce::StringArray used;
        for (auto& sp : inst->samples)
        {
            auto base = juce::File::createLegalFileName (juce::File (sp->id.replaceCharacter ('\\', '/')).getFileNameWithoutExtension());
            if (base.isEmpty()) base = "sempl";
            auto fileName = base + ".wav";
            for (int k = 2; used.contains (fileName, true); ++k) fileName = base + " " + juce::String (k) + ".wav";
            used.add (fileName);
            auto light = std::make_shared<SampleData>();
            light->id = fileName;
            out.samples.push_back (light);

            const auto f = folder.getChildFile (fileName);
            if (! f.existsAsFile())
            {
                const int len = sp->length();
                const bool mono = sp->audio().getNumChannels() < 2
                               || juce::FloatVectorOperations::findMaximum (sp->audio().getReadPointer (0), len) == juce::FloatVectorOperations::findMaximum (sp->audio().getReadPointer (1), len)
                                  && std::memcmp (sp->audio().getReadPointer (0), sp->audio().getReadPointer (1), sizeof (float) * (size_t) len) == 0;
                std::unordered_map<juce::String, juce::String> meta;
                if (sp->fileRootKey >= 0) meta["MidiUnityNote"] = juce::String (sp->fileRootKey);
                if (sp->hasLoop && sp->loopEnd > sp->loopStart + 16)
                {
                    meta["NumSampleLoops"] = "1";
                    meta["Loop0Type"] = "0";
                    meta["Loop0Start"] = juce::String (sp->loopStart);
                    meta["Loop0End"] = juce::String (sp->loopEnd - 1);
                }
                std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
                auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sp->sampleRate).withNumChannels (mono ? 1 : 2)
                                                      .withBitsPerSample (24).withMetadataValues (meta));
                if (w == nullptr) return false;
                w->writeFromAudioSampleBuffer (sp->audio(), 0, len);
            }
        }
        // presemplovan Kronos zvuk: oznaka (release, slojevi) ide uz semplove
        if (inst->type == SourceType::WavFolder)
            for (auto* tagName : { "kronos-ms.txt" })
                if (auto t = inst->source.getChildFile (tagName); t.existsAsFile())
                    t.copyFileTo (folder.getChildFile (tagName));
        return out.saveMap();
    }

    LiveSamplerProcessor& proc;
    juce::File dest;
    std::vector<UserProgram> users;
    juce::File data;
    juce::CriticalSection lock;
    juce::String text;
};

bool LiveSamplerProcessor::startBackup (const juce::File& destFolder)
{
    if (isBackupRunning() || ! destFolder.createDirectory()) return false;
    saveNow();
    const auto snapshot = destFolder.getChildFile ("podaci-original.tmp");
    getDataFile().copyFileTo (snapshot);
    backupThread = std::make_unique<BackupThread> (*this, destFolder, users, snapshot);
    backupThread->startThread();
    return true;
}

bool LiveSamplerProcessor::isBackupRunning() const { return backupThread != nullptr && backupThread->isThreadRunning(); }

juce::String LiveSamplerProcessor::getBackupStatus() const
{
    if (auto* b = dynamic_cast<BackupThread*> (backupThread.get())) return b->status();
    return {};
}

bool LiveSamplerProcessor::restoreBackup (const juce::File& backupFolder, juce::String& report)
{
    const auto dataFile = backupFolder.getChildFile (kBackupData);
    auto xml = juce::XmlDocument::parse (dataFile);
    if (xml == nullptr || ! xml->hasTagName ("LiveSamplerData"))
    {
        report = T ("This folder has no backup (file \"") + juce::String (kBackupData) + "\").";
        return false;
    }
    // biblioteka iz backup-a + postojeci folderi ovog racunara
    auto* folders = xml->getChildByName ("Folders");
    if (folders == nullptr) folders = xml->createNewChildElement ("Folders");
    folders->deleteAllChildElements();
    folders->createNewChildElement ("Folder")->setAttribute ("path", backupFolder.getChildFile (kBackupLib).getFullPathName());
    for (auto& f : libraryFolders)
        if (f != backupFolder.getChildFile (kBackupLib))
            folders->createNewChildElement ("Folder")->setAttribute ("path", f.getFullPathName());

    // dosadasnji podaci ostaju sacuvani pored
    const auto cur = getDataFile();
    if (cur.existsAsFile())
        cur.copyFileTo (cur.getSiblingFile ("LiveSampler-podaci pre vracanja " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M") + ".xml"));
    if (! xml->writeTo (cur))
    {
        report = T ("Cannot write the data to ") + cur.getFullPathName();
        return false;
    }
    restorePending = true;
    report = T ("Backup restored. Close and restart Pikaso Workstation.\nThe previous data was kept next to it (LiveSampler-podaci pre vracanja ...xml).");
    return true;
}

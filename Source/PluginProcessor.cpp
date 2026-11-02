#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Text.h"

juce::Colour LiveSamplerProcessor::partColour (int i)
{
    static const juce::uint32 c[] = { 0xffff8a3d, 0xff3dc5ff, 0xff7ddf64, 0xffff5c8a,
                                      0xffc58aff, 0xffffd23d, 0xff3dffd0, 0xffff9be0 };
    return juce::Colour (c[(size_t) ((i % 8) + 8) % 8]);
}

//==============================================================================
LiveSamplerProcessor::LiveSamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    terca = TercaSettings::makeDefault();
    loadData();
    rebuildConfig();
    if (! libraryFolders.isEmpty())
        rescanLibrary();
    startTimer (500);
}

LiveSamplerProcessor::~LiveSamplerProcessor()
{
    alive->store (false);
    stopTimer();
    if (saveNeeded)
        saveNow();
    loadPool.removeAllJobs (true, 10000);
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
    juce::XmlElement root ("LiveSamplerData");
    root.setAttribute ("version", 2);
    root.setAttribute ("c3Is60", c3Is60);
    root.setAttribute ("globalDb", (double) globalDb);
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
    currentSlot = xml->getIntAttribute ("currentSlot", -1);
    currentBank = juce::jlimit (0, kNumPerfBanks - 1, xml->getIntAttribute ("currentBank", 0));
    kronosFile = juce::File (xml->getStringAttribute ("kronosPcg"));

    if (auto* folders = xml->getChildByName ("Folders"))
        for (auto* f : folders->getChildWithTagNameIterator ("Folder"))
            libraryFolders.add (juce::File (f->getStringAttribute ("path")));

    if (auto* us = xml->getChildByName ("Users"))
        for (auto* u : us->getChildWithTagNameIterator ("User"))
            users.push_back (UserProgram::fromXml (*u));

    if (auto* ps = xml->getChildByName ("Slots"))
        for (auto* p : ps->getChildWithTagNameIterator ("Performance"))
        {
            const int i = p->getIntAttribute ("slot", -1);
            if (i >= 0 && i < kNumPerfSlots)
                slots[(size_t) i] = Performance::fromXml (*p);
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
        for (auto& part : p.parts)
            if (part.on)
                if (const int u = findUser (part.user); u >= 0)
                    for (auto& n : users[(size_t) u].usedInstruments())
                        names.addIfNotAlreadyThere (n);
    };

    addPerf (perf);
    if (includeBank)
        for (int i = 0; i < kPerfPerBank; ++i)
            if (const auto& s = slots[(size_t) (currentBank * kPerfPerBank + i)]; s.used)
                addPerf (s);
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
        installInstrument (name, cur->withZones (std::move (zones)));
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
            lastWarning = name + T (": nema upotrebljivih semplova");
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
                if (pr.user.osc[(size_t) o].on)
                    pr.inst[(size_t) o] = getLoadedInstrument (pr.user.osc[(size_t) o].instrument);
        }
        pr.active = pr.part.on && u >= 0;
    }
    cfg->master = perf.master;
    cfg->globalDb = globalDb;
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

void LiveSamplerProcessor::timerCallback()
{
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
    const int prog = pendingProgram.exchange (-1);
    if (prog >= 0 && prog < kNumPerfSlots)
        recallPerformance (prog);

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

int LiveSamplerProcessor::createUserFromKronos (const Kronos::Program& prog)
{
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
        sb.vibDepth = kSemis (q.pitchLfo1Int) * 100.0f;
        sb.vibDelay = 0.0f;
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
            if (! zone.user || lo > hi || ms >= kMsPerOsc) continue;

            const auto inst = library.findKronosMultisample (zone.uuid, zone.ms);
            if (inst.isEmpty()) continue;

            auto& os = u.osc[(size_t) (o * kMsPerOsc + ms++)];
            ++n;
            os.on = true;
            os.instrument = inst;
            os.block = o;
            os.velLow = lo;
            os.velHigh = hi;
            os.fadeIn = fadeIn;
            os.fadeOut = fadeOut;
            os.fadeCurve = oscCurve;
            os.keyLow = juce::jmin (q.keyLow, q.keyHigh);
            os.keyHigh = juce::jmax (q.keyLow, q.keyHigh);
            os.transpose = q.transpose;
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
    u.portaOn = q1.portaOn != 0;
    u.portaFingered = q1.portaFingered != 0;
    u.portaConstRate = q1.portaConstTime == 0;
    u.portamento = q1.portaTime <= 0 ? 0.0f : 0.002f * std::exp (0.085f * (float) q1.portaTime);
    u.bendUp = juce::jlimit (0, 24, std::abs (q1.bendUp));
    u.bendDown = juce::jlimit (0, 24, std::abs (q1.bendDown));

    const int idx = createUser (prog.name.isNotEmpty() ? prog.name : prog.id(), &u);
    loadNeededInstruments();
    for (auto& os : users[(size_t) idx].osc)
        if (os.on) requestInstrument (os.instrument, nullptr);
    return idx;
}

int LiveSamplerProcessor::userForKronosProgram (const Kronos::Program& p)
{
    const int existing = findUser (p.name);
    if (existing >= 0)
        return existing;
    return createUserFromKronos (p);
}

juce::StringArray LiveSamplerProcessor::loadKronosCombi (const Kronos::Combi& c)
{
    juce::StringArray skipped;
    if (kronosPcg == nullptr)
        return skipped;

    auto p = perf;
    p.name = c.name.isNotEmpty() ? c.name : c.id();
    for (int t = 0; t < 16 && t < kNumParts; ++t)
    {
        const auto& tm = c.timbres[(size_t) t];
        auto& part = p.parts[(size_t) t];
        part = PerfPart();
        if (! tm.play)
            continue;

        const auto* prog = kronosPcg->findProgram (tm.programBank, tm.programNumber);
        const int u = prog != nullptr && prog->hd1 && prog->usesUser() ? userForKronosProgram (*prog) : -1;
        if (u < 0)
        {
            skipped.add ("Timbre " + juce::String (t + 1) + ": "
                         + (prog != nullptr ? prog->id() + " " + prog->name : juce::String ("program ") + juce::String (tm.programNumber))
                         + (prog != nullptr && ! prog->usesUser() ? " (fabrički ROM)" : ""));
            continue;
        }

        part.on = true;
        part.user = users[(size_t) u].name;
        part.volumeDb = tm.volume <= 0 ? -60.0f : juce::jlimit (-60.0f, 6.0f, 40.0f * std::log10 ((float) tm.volume / 127.0f));
        part.pan = juce::jlimit (-1.0f, 1.0f, ((float) tm.pan - 64.0f) / 64.0f);
        part.transpose = tm.transpose;
        part.detuneCents = (float) tm.detuneCents;
        part.keyLow = juce::jmin (tm.keyLow, tm.keyHigh);
        part.keyHigh = juce::jmax (tm.keyLow, tm.keyHigh);
        part.velLow = juce::jmin (tm.velLow, tm.velHigh);
        part.velHigh = juce::jmax (tm.velLow, tm.velHigh);
        // timbre na globalnom kanalu Kronosa svira sa klavijature kao Gch -> svi kanali
        part.midiChannel = tm.channel == kronosPcg->globalChannel ? 0 : tm.channel;
        part.reverbSend = (float) tm.send1 / 127.0f;
        part.delaySend = (float) tm.send2 / 127.0f;
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

//==============================================================================
//  Performans
//==============================================================================
void LiveSamplerProcessor::updatePerformance (const Performance& p)
{
    perf = p;
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
}

void LiveSamplerProcessor::recallPerformance (int slot)
{
    if (slot < 0 || slot >= kNumPerfSlots || ! slots[(size_t) slot].used)
        return;
    perf = slots[(size_t) slot];
    currentSlot = slot;
    currentBank = slot / kPerfPerBank;
    perfDirty = false;
    markSave();
    rebuildConfig();
    loadNeededInstruments();
    sendChangeMessage();
}

void LiveSamplerProcessor::setCurrentBank (int b)
{
    currentBank = juce::jlimit (0, kNumPerfBanks - 1, b);
    loadNeededInstruments();
    sendChangeMessage();
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
    for (auto& p : parts)
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
        capturedMask |= 1 << (note % 12);
        tryRecognize (capturedMask | heldMask(), c);
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

        const int shift = part.transpose + part.octave * 12;
        parts[p].noteOn (note, note + shift, vel);
        used |= (juce::uint16) (1 << p);

        if (tOn && part.terca && ! tercaCapturing)
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
                const int hv = juce::jlimit (1, 127, juce::roundToInt ((float) vel * c.terca.velocity));
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

void LiveSamplerProcessor::routeNoteOff (int ch, int note)
{
    if (heldCount[note] > 0)
        --heldCount[note];

    for (int p = 0; p < kNumParts; ++p)
    {
        if (routed[ch][note] & (1 << p))
            parts[p].noteOff (note);
        if (harmonized[ch][note] & (1 << p))
            parts[p].noteOff (128 + note);
    }
    routed[ch][note] = 0;
    harmonized[ch][note] = 0;
}

void LiveSamplerProcessor::handleMidi (const juce::MidiMessage& m, const EngineConfig& c)
{
    const int ch = juce::jlimit (0, 15, m.getChannel() - 1);
    const auto& lt = c.terca.learn;

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
        tercaTrigger (m.getControllerValue() >= 64, c);
        return;
    }
    if ((m.isNoteOn() || m.isNoteOff()) && lt.matchesNote (m.getChannel(), m.getNoteNumber()))
    {
        tercaTrigger (m.isNoteOn(), c);
        return;
    }

    if (m.isNoteOn())
    {
        routeNoteOn (ch, m.getNoteNumber(), m.getVelocity(), c);
    }
    else if (m.isNoteOff())
    {
        routeNoteOff (ch, m.getNoteNumber());
    }
    else if (m.isSustainPedalOn() || m.isSustainPedalOff())
    {
        for (auto& p : parts)
            p.setSustain (m.isSustainPedalOn());
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
        std::memset (routed, 0, sizeof (routed));
        std::memset (harmonized, 0, sizeof (harmonized));
        std::memset (heldCount, 0, sizeof (heldCount));
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

    {
        const juce::SpinLock::ScopedLockType sl (configLock);
        if (audioConfig != pendingConfig)
            audioConfig = pendingConfig;
    }
    if (audioConfig == nullptr)
        return;
    const auto& c = *audioConfig;

    keyboardState.processNextMidiBuffer (midi, 0, n, true);

    if (panicRequested.exchange (false))
    {
        for (auto& p : parts)
            p.allNotesOff (true);
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
            parts[p].beginBlock (&c.parts[(size_t) p], len);

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

        auto ms = c.master;
        ms.masterDb += c.globalDb;
        master.process (mainBuf, reverbBuf, delayBuf, len, ms);

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

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);

    int voicesNow = 0;
    for (auto& p : parts)
        voicesNow += p.getNumActiveVoices();
    activeVoices = voicesNow;
}

void LiveSamplerProcessor::panic()
{
    panicRequested = true;
}

juce::String LiveSamplerProcessor::getStatusText() const
{
    if (libraryFolders.isEmpty())
        return T ("Dodaj folder sa semplovima (PODEŠAVANJA)");
    if (scanning)
        return T ("Pretražujem foldere...");
    if (! pendingLoads.empty())
        return T ("Učitavam: ") + pendingLoads.begin()->first.fromLastOccurrenceOf ("/", false, false)
             + (pendingLoads.size() > 1 ? " (+" + juce::String ((int) pendingLoads.size() - 1) + ")" : juce::String());

    size_t bytes = 0;
    for (auto& kv : cache)
        bytes += kv.second->memoryBytes();

    auto s = juce::String (instrumentList.size()) + T (" instrumenata  |  ") + juce::String ((double) bytes / (1024.0 * 1024.0), 0) + " MB";
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

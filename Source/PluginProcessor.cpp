#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Text.h"

using APF = juce::AudioParameterFloat;
using APB = juce::AudioParameterBool;
using API = juce::AudioParameterInt;
using APC = juce::AudioParameterChoice;

static juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
{
    juce::NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (centre);
    return r;
}

static juce::AudioParameterFloatAttributes unit (const char* label, int decimals = 1)
{
    return juce::AudioParameterFloatAttributes()
        .withLabel (label)
        .withStringFromValueFunction ([decimals] (float v, int) { return juce::String (v, decimals); });
}

static juce::AudioParameterFloatAttributes percent()
{
    return juce::AudioParameterFloatAttributes()
        .withLabel ("%")
        .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)); });
}

static juce::AudioParameterFloatAttributes seconds()
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int)
        {
            return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms"
                            : juce::String (v, 2) + " s";
        });
}

static juce::AudioParameterFloatAttributes hertz()
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int)
        {
            return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz"
                               : juce::String (v / 1000.0f, 1) + " kHz";
        });
}

juce::AudioProcessorValueTreeState::ParameterLayout LiveSamplerProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int p = 0; p < numParts; ++p)
    {
        auto g = std::make_unique<juce::AudioProcessorParameterGroup> ("part" + juce::String (p), partName (p), "|");
        auto id = [p] (const char* n) { return juce::ParameterID { pid (p, n), 1 }; };
        auto nm = [p] (const char* n) { return partName (p) + " " + T (n); };

        g->addChild (std::make_unique<APB> (id ("on"), nm ("Uključen"), p != 1));
        g->addChild (std::make_unique<APF> (id ("vol"), nm ("Jačina"), skewed (-60.0f, 6.0f, -12.0f), p == 1 ? -6.0f : 0.0f, unit ("dB")));
        g->addChild (std::make_unique<APF> (id ("pan"), nm ("Pan"), juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f,
                                            juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                            {
                                                const int x = juce::roundToInt (v * 50.0f);
                                                return x == 0 ? juce::String ("C") : (x < 0 ? "L" + juce::String (-x) : "R" + juce::String (x));
                                            })));
        g->addChild (std::make_unique<API> (id ("oct"), nm ("Oktava"), -3, 3, 0));
        g->addChild (std::make_unique<API> (id ("trans"), nm ("Transpozicija"), -12, 12, 0));
        g->addChild (std::make_unique<APF> (id ("fine"), nm ("Fino štimovanje"), juce::NormalisableRange<float> (-100.0f, 100.0f), 0.0f, unit ("c", 0)));
        g->addChild (std::make_unique<APF> (id ("velsens"), nm ("Velocity"), juce::NormalisableRange<float> (0.0f, 1.0f), 0.8f, percent()));
        g->addChild (std::make_unique<APB> (id ("mono"), nm ("Mono"), false));
        g->addChild (std::make_unique<APF> (id ("porta"), nm ("Portamento"), skewed (0.0f, 2.0f, 0.25f), 0.0f, seconds()));
        g->addChild (std::make_unique<APF> (id ("att"), nm ("Attack"), skewed (0.001f, 5.0f, 0.3f), 0.001f, seconds()));
        g->addChild (std::make_unique<APF> (id ("dec"), nm ("Decay"), skewed (0.01f, 5.0f, 0.5f), 0.5f, seconds()));
        g->addChild (std::make_unique<APF> (id ("sus"), nm ("Sustain"), juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f, percent()));
        g->addChild (std::make_unique<APF> (id ("rel"), nm ("Release"), skewed (0.01f, 8.0f, 0.6f), 0.3f, seconds()));
        g->addChild (std::make_unique<APF> (id ("cutoff"), nm ("Cutoff"), skewed (40.0f, 20000.0f, 1500.0f), 20000.0f, hertz()));
        g->addChild (std::make_unique<APF> (id ("reso"), nm ("Rezonanca"), juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
        g->addChild (std::make_unique<APC> (id ("fxtype"), nm ("Insert efekat"), InsertFx::getTypeNames(), 0));
        g->addChild (std::make_unique<APF> (id ("fxrate"), nm ("FX brzina"), juce::NormalisableRange<float> (0.0f, 1.0f), 0.3f, percent()));
        g->addChild (std::make_unique<APF> (id ("fxdepth"), nm ("FX dubina"), juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent()));
        g->addChild (std::make_unique<APF> (id ("fxmix"), nm ("FX mix"), juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent()));
        g->addChild (std::make_unique<APF> (id ("rev"), nm ("Reverb send"), juce::NormalisableRange<float> (0.0f, 1.0f), 0.2f, percent()));
        g->addChild (std::make_unique<APF> (id ("dly"), nm ("Delay send"), juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
        g->addChild (std::make_unique<API> (id ("bend"), nm ("Pitch bend opseg"), 0, 12, 2));

        layout.add (std::move (g));
    }

    auto gid = [] (const char* n) { return juce::ParameterID { n, 1 }; };

    layout.add (std::make_unique<APB> (gid ("split_on"), "Split", false));
    layout.add (std::make_unique<API> (gid ("split_pt"), "Split tacka", 24, 96, 54,
        juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int)
        {
            return juce::MidiMessage::getMidiNoteName (v, true, true, 3);
        })));
    layout.add (std::make_unique<API> (gid ("lower_ch"), "Donji MIDI kanal", 0, 16, 0,
        juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int)
        {
            return v == 0 ? T ("Split") : "Kanal " + juce::String (v);
        })));

    layout.add (std::make_unique<APF> (gid ("rev_size"), "Reverb velicina", juce::NormalisableRange<float> (0.0f, 1.0f), 0.55f, percent()));
    layout.add (std::make_unique<APF> (gid ("rev_damp"), "Reverb prigusenje", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent()));
    layout.add (std::make_unique<APF> (gid ("rev_level"), "Reverb nivo", juce::NormalisableRange<float> (0.0f, 1.0f), 0.7f, percent()));
    layout.add (std::make_unique<APF> (gid ("dly_time"), "Delay vreme", skewed (20.0f, 1500.0f, 350.0f), 350.0f, unit ("ms", 0)));
    layout.add (std::make_unique<APF> (gid ("dly_fb"), "Delay ponavljanje", juce::NormalisableRange<float> (0.0f, 0.95f), 0.35f, percent()));
    layout.add (std::make_unique<APF> (gid ("dly_level"), "Delay nivo", juce::NormalisableRange<float> (0.0f, 1.0f), 0.6f, percent()));
    layout.add (std::make_unique<APF> (gid ("eq_low"), "EQ bas", juce::NormalisableRange<float> (-12.0f, 12.0f), 0.0f, unit ("dB")));
    layout.add (std::make_unique<APF> (gid ("eq_mid"), "EQ sredina", juce::NormalisableRange<float> (-12.0f, 12.0f), 0.0f, unit ("dB")));
    layout.add (std::make_unique<APF> (gid ("eq_high"), "EQ visoki", juce::NormalisableRange<float> (-12.0f, 12.0f), 0.0f, unit ("dB")));
    layout.add (std::make_unique<APF> (gid ("comp_thr"), "Kompresor prag", juce::NormalisableRange<float> (-40.0f, 0.0f), -12.0f, unit ("dB")));
    layout.add (std::make_unique<APF> (gid ("comp_ratio"), "Kompresor odnos", skewed (1.0f, 10.0f, 3.0f), 1.0f, unit (":1")));
    layout.add (std::make_unique<APF> (gid ("master_vol"), "Master", skewed (-60.0f, 6.0f, -12.0f), 0.0f, unit ("dB")));

    return layout;
}

//==============================================================================
LiveSamplerProcessor::LiveSamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "LiveSampler", createLayout())
{
    for (int p = 0; p < numParts; ++p)
    {
        auto get = [this, p] (const char* n) { return apvts.getRawParameterValue (pid (p, n)); };
        auto& x = pp[p];
        x.on = get ("on"); x.vol = get ("vol"); x.pan = get ("pan"); x.oct = get ("oct");
        x.trans = get ("trans"); x.fine = get ("fine"); x.velsens = get ("velsens");
        x.mono = get ("mono"); x.porta = get ("porta"); x.att = get ("att"); x.dec = get ("dec");
        x.sus = get ("sus"); x.rel = get ("rel"); x.cutoff = get ("cutoff"); x.reso = get ("reso");
        x.fxtype = get ("fxtype"); x.fxrate = get ("fxrate"); x.fxdepth = get ("fxdepth");
        x.fxmix = get ("fxmix"); x.rev = get ("rev"); x.dly = get ("dly"); x.bend = get ("bend");
    }
    splitOn = apvts.getRawParameterValue ("split_on");
    splitPoint = apvts.getRawParameterValue ("split_pt");
    lowerCh = apvts.getRawParameterValue ("lower_ch");
    revSize = apvts.getRawParameterValue ("rev_size");
    revDamp = apvts.getRawParameterValue ("rev_damp");
    revLevel = apvts.getRawParameterValue ("rev_level");
    dlyTime = apvts.getRawParameterValue ("dly_time");
    dlyFb = apvts.getRawParameterValue ("dly_fb");
    dlyLevel = apvts.getRawParameterValue ("dly_level");
    eqLow = apvts.getRawParameterValue ("eq_low");
    eqMid = apvts.getRawParameterValue ("eq_mid");
    eqHigh = apvts.getRawParameterValue ("eq_high");
    compThr = apvts.getRawParameterValue ("comp_thr");
    compRatio = apvts.getRawParameterValue ("comp_ratio");
    masterVol = apvts.getRawParameterValue ("master_vol");

    juce::PropertiesFile::Options opts;
    opts.applicationName = "LiveSampler";
    opts.filenameSuffix = "settings";
    opts.folderName = "LiveSampler";
    opts.osxLibrarySubFolder = "Application Support";
    appProps.setStorageParameters (opts);

    auto* settings = appProps.getUserSettings();
    c3Is60 = settings->getBoolValue ("c3Is60", true);
    registers.loadFrom (getRegistersFile());

    juce::File folder (settings->getValue ("libraryFolder"));
    if (folder.isDirectory())
        setLibraryFolder (folder);

    startTimer (4000);
}

LiveSamplerProcessor::~LiveSamplerProcessor()
{
    alive->store (false);
    stopTimer();
    loadPool.removeAllJobs (true, 10000);
}

juce::File LiveSamplerProcessor::getRegistersFile()
{
    return appProps.getUserSettings()->getFile().getSiblingFile ("registers.xml");
}

bool LiveSamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

//==============================================================================
//  Biblioteka i ucitavanje instrumenata (message nit)
//==============================================================================
void LiveSamplerProcessor::setLibraryFolder (const juce::File& folder)
{
    libraryFolder = folder;
    appProps.getUserSettings()->setValue ("libraryFolder", folder.getFullPathName());
    appProps.saveIfNeeded();
    rescanLibrary();
}

void LiveSamplerProcessor::rescanLibrary()
{
    scanning = true;
    sendChangeMessage();

    auto folder = libraryFolder;
    auto aliveFlag = alive;
    loadPool.addJob ([this, folder, aliveFlag]
    {
        library.scan (folder);
        auto names = library.getInstrumentNames();

        juce::MessageManager::callAsync ([this, names, aliveFlag]
        {
            if (! aliveFlag->load())
                return;

            scanning = false;
            instrumentList = names;

            for (int p = 0; p < numParts; ++p)
            {
                auto n = instrumentNames[p];
                if (n.isEmpty() && p == 0 && ! names.isEmpty())
                    n = names[0];
                if (n.isNotEmpty())
                    selectInstrument (p, n);
            }

            preloadRegisterInstruments();
            sendChangeMessage();
        });
    });
}

void LiveSamplerProcessor::requestInstrument (const juce::String& name, LoadCallback cb)
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
    loadPool.addJob ([this, name, epoch, c3, aliveFlag]
    {
        if (! aliveFlag->load())
            return;

        auto inst = library.loadInstrument (name, c3);

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
            lastWarning = name + ": " + inst->warnings[0]
                        + (inst->warnings.size() > 1 ? " (+" + juce::String (inst->warnings.size() - 1) + ")" : juce::String());
        if (inst->zones.empty())
            lastWarning = name + T (": nema upotrebljivih semplova");

        cache[name] = inst;
    }

    for (auto& cb : callbacks)
        if (cb) cb (inst);

    sendChangeMessage();
}

void LiveSamplerProcessor::selectInstrument (int part, const juce::String& name)
{
    if (part < 0 || part >= numParts)
        return;

    instrumentNames[part] = name;
    const int requestId = ++partRequestId[part];
    sendChangeMessage();

    if (name.isEmpty())
    {
        parts[part].setInstrument (nullptr);
        return;
    }

    requestInstrument (name, [this, part, requestId] (InstrumentPtr inst)
    {
        if (partRequestId[part] == requestId)
            parts[part].setInstrument (inst);
    });
}

void LiveSamplerProcessor::preloadRegisterInstruments()
{
    for (auto& n : registers.getAllInstrumentNames())
        if (instrumentList.contains (n))
            requestInstrument (n, nullptr);
}

void LiveSamplerProcessor::setC3Is60 (bool shouldBe)
{
    if (c3Is60 == shouldBe)
        return;

    c3Is60 = shouldBe;
    appProps.getUserSettings()->setValue ("c3Is60", shouldBe);
    appProps.saveIfNeeded();

    // ponovo ucitaj sve sa novim mapiranjem oktava
    ++loadEpoch;
    for (auto& kv : cache)
        graveyard.push_back (kv.second);
    cache.clear();
    pendingLoads.clear();

    for (int p = 0; p < numParts; ++p)
        selectInstrument (p, instrumentNames[p]);
    preloadRegisterInstruments();
}

void LiveSamplerProcessor::timerCallback()
{
    // oslobodi memoriju instrumenata koje vise niko ne koristi
    juce::StringArray keep = registers.getAllInstrumentNames();
    for (auto& n : instrumentNames)
        keep.add (n);

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
}

juce::String LiveSamplerProcessor::getStatusText() const
{
    if (! libraryFolder.isDirectory())
        return T ("Izaberi folder sa semplovima");
    if (scanning)
        return T ("Pretražujem folder...");
    if (! pendingLoads.empty())
        return T ("Učitavam: ") + pendingLoads.begin()->first
             + (pendingLoads.size() > 1 ? " (+" + juce::String ((int) pendingLoads.size() - 1) + ")" : juce::String());

    size_t bytes = 0;
    for (auto& kv : cache)
        bytes += kv.second->memoryBytes();

    auto s = juce::String (instrumentList.size()) + T (" instrumenata  |  u memoriji: ")
           + juce::String ((int) cache.size()) + " (" + juce::String ((double) bytes / (1024.0 * 1024.0), 0) + " MB)";
    if (lastWarning.isNotEmpty())
        s << T ("  |  ⚠ ") << lastWarning;
    return s;
}

//==============================================================================
//  Registri
//==============================================================================
static bool isRegisterParam (const juce::String& id)
{
    return id.startsWith ("p") || id.startsWith ("split") || id.startsWith ("lower");
}

void LiveSamplerProcessor::storeRegister (int index)
{
    auto& slot = registers[index];
    slot = Registers::Slot();
    slot.used = true;

    for (auto* param : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (isRegisterParam (rp->getParameterID()))
                slot.values.set (rp->getParameterID(), rp->getValue());

    for (int p = 0; p < numParts; ++p)
        slot.instruments[p] = instrumentNames[p];

    registers.saveTo (getRegistersFile());
    currentRegister = index;
    sendChangeMessage();
}

void LiveSamplerProcessor::recallRegister (int index)
{
    auto& slot = registers[index];
    if (! slot.used)
        return;

    for (auto& nv : slot.values)
        if (auto* rp = apvts.getParameter (nv.name.toString()))
            rp->setValueNotifyingHost ((float) (double) nv.value);

    for (int p = 0; p < numParts; ++p)
        if (slot.instruments[p] != instrumentNames[p])
            selectInstrument (p, slot.instruments[p]);

    currentRegister = index;
    sendChangeMessage();
}

void LiveSamplerProcessor::handleAsyncUpdate()
{
    const int prog = pendingProgram.exchange (-1);
    if (prog >= 0 && prog < Registers::numSlots)
        recallRegister (prog);
}

void LiveSamplerProcessor::panic()
{
    panicRequested = true;
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
    keyboardState.reset();
}

void LiveSamplerProcessor::readSettings (PartSettings* ps, MasterSettings& ms)
{
    for (int p = 0; p < numParts; ++p)
    {
        auto& x = pp[p];
        auto& s = ps[p];
        s.enabled = x.on->load() > 0.5f;
        s.volumeDb = x.vol->load();
        s.pan = x.pan->load();
        s.octave = (int) std::lround (x.oct->load());
        s.transpose = (int) std::lround (x.trans->load());
        s.fineCents = x.fine->load();
        s.velSens = x.velsens->load();
        s.mono = x.mono->load() > 0.5f;
        s.portamento = x.porta->load();
        s.attack = x.att->load();
        s.decay = x.dec->load();
        s.sustain = x.sus->load();
        s.release = x.rel->load();
        s.cutoff = x.cutoff->load();
        s.resonance = x.reso->load();
        s.fxType = (int) std::lround (x.fxtype->load());
        s.fxRate = x.fxrate->load();
        s.fxDepth = x.fxdepth->load();
        s.fxMix = x.fxmix->load();
        s.reverbSend = x.rev->load();
        s.delaySend = x.dly->load();
        s.bendRange = (int) std::lround (x.bend->load());
    }

    ms.reverbSize = revSize->load();
    ms.reverbDamping = revDamp->load();
    ms.reverbLevel = revLevel->load();
    ms.delayTimeMs = dlyTime->load();
    ms.delayFeedback = dlyFb->load();
    ms.delayLevel = dlyLevel->load();
    ms.eqLowDb = eqLow->load();
    ms.eqMidDb = eqMid->load();
    ms.eqHighDb = eqHigh->load();
    ms.compThresholdDb = compThr->load();
    ms.compRatio = compRatio->load();
    ms.masterDb = masterVol->load();
}

void LiveSamplerProcessor::handleMidi (const juce::MidiMessage& m, const PartSettings* ps)
{
    const int ch = juce::jlimit (0, 15, m.getChannel() - 1);

    if (m.isNoteOn())
    {
        const int note = m.getNoteNumber();
        const int lch = (int) std::lround (lowerCh->load());
        const bool split = splitOn->load() > 0.5f;
        const int splitPt = (int) std::lround (splitPoint->load());

        juce::uint8 mask;
        if (lch > 0 && m.getChannel() == lch)                 mask = 4;
        else if (split && note < splitPt && ps[2].enabled)   mask = 4;
        else                                                 mask = 1 | 2;

        juce::uint8 used = 0;
        for (int p = 0; p < numParts; ++p)
        {
            if ((mask & (1 << p)) && ps[p].enabled)
            {
                parts[p].noteOn (note, m.getVelocity());
                used |= (juce::uint8) (1 << p);
            }
        }
        routed[ch][note] |= used;
    }
    else if (m.isNoteOff())
    {
        const int note = m.getNoteNumber();
        for (int p = 0; p < numParts; ++p)
            if (routed[ch][note] & (1 << p))
                parts[p].noteOff (note);
        routed[ch][note] = 0;
    }
    else if (m.isSustainPedalOn() || m.isSustainPedalOff())
    {
        for (auto& p : parts)
            p.setSustain (m.isSustainPedalOn());
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
    }
    else if (m.isProgramChange())
    {
        pendingProgram = m.getProgramChangeNumber();
        triggerAsyncUpdate();
    }
}

void LiveSamplerProcessor::processChunk (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi,
                                         int chunkStart, int len, const PartSettings* ps, const MasterSettings& ms)
{
    for (int p = 0; p < numParts; ++p)
        parts[p].beginBlock (ps[p], len);

    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = meta.samplePosition - chunkStart;
        if (t < 0) continue;
        if (t >= len) break;

        if (t > pos)
        {
            for (auto& p : parts)
                p.renderVoices (pos, t - pos);
            pos = t;
        }
        handleMidi (meta.getMessage(), ps);
    }

    for (auto& p : parts)
        p.renderVoices (pos, len - pos);

    mainBuf.clear (0, len);
    reverbBuf.clear (0, len);
    delayBuf.clear (0, len);

    for (auto& p : parts)
        p.endBlock (mainBuf, reverbBuf, delayBuf, len);

    master.process (mainBuf, reverbBuf, delayBuf, len, ms);

    if (out.getNumChannels() >= 2)
    {
        out.copyFrom (0, chunkStart, mainBuf, 0, 0, len);
        out.copyFrom (1, chunkStart, mainBuf, 1, 0, len);
    }
    else if (out.getNumChannels() == 1)
    {
        out.copyFrom (0, chunkStart, mainBuf, 0, 0, len);
        out.addFrom (0, chunkStart, mainBuf, 1, 0, len);
        out.applyGain (0, chunkStart, len, 0.5f);
    }
}

void LiveSamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();

    keyboardState.processNextMidiBuffer (midi, 0, n, true);

    if (panicRequested.exchange (false))
    {
        for (auto& p : parts)
            p.allNotesOff (true);
        std::memset (routed, 0, sizeof (routed));
    }

    PartSettings ps[numParts];
    MasterSettings ms;
    readSettings (ps, ms);

    for (int start = 0; start < n; start += maxBlock)
        processChunk (buffer, midi, start, juce::jmin (maxBlock, n - start), ps, ms);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);
}

//==============================================================================
void LiveSamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (int p = 0; p < numParts; ++p)
        state.setProperty ("inst" + juce::String (p), instrumentNames[p], nullptr);
    state.setProperty ("libraryFolder", libraryFolder.getFullPathName(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LiveSamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);

    for (int p = 0; p < numParts; ++p)
        instrumentNames[p] = state.getProperty ("inst" + juce::String (p)).toString();

    juce::File folder (state.getProperty ("libraryFolder").toString());
    if (folder.isDirectory() && folder != libraryFolder)
        setLibraryFolder (folder);   // posle skeniranja ucitava instrumentNames
    else
        for (int p = 0; p < numParts; ++p)
            selectInstrument (p, instrumentNames[p]);
}

//==============================================================================
juce::AudioProcessorEditor* LiveSamplerProcessor::createEditor()
{
    return new LiveSamplerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LiveSamplerProcessor();
}

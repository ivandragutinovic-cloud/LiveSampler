#include "Model.h"
#include "Text.h"

//==============================================================================
//  User
//==============================================================================
static void envToXml (const EnvSettings& e, juce::XmlElement& x)
{
    x.setAttribute ("start", (double) e.start);
    x.setAttribute ("aTime", (double) e.attackTime);
    x.setAttribute ("aLevel", (double) e.attackLevel);
    x.setAttribute ("dTime", (double) e.decayTime);
    x.setAttribute ("bLevel", (double) e.breakLevel);
    x.setAttribute ("sTime", (double) e.slopeTime);
    x.setAttribute ("sustain", (double) e.sustain);
    x.setAttribute ("rTime", (double) e.releaseTime);
    x.setAttribute ("rLevel", (double) e.releaseLevel);
}

static EnvSettings envFromXml (const juce::XmlElement* x, EnvSettings e)
{
    if (x == nullptr) return e;
    auto f = [x] (const char* n, float def) { return (float) x->getDoubleAttribute (n, def); };
    e.start = f ("start", e.start);
    e.attackTime = f ("aTime", e.attackTime);
    e.attackLevel = f ("aLevel", e.attackLevel);
    e.decayTime = f ("dTime", e.decayTime);
    e.breakLevel = f ("bLevel", e.breakLevel);
    e.slopeTime = f ("sTime", e.slopeTime);
    e.sustain = f ("sustain", e.sustain);
    e.releaseTime = f ("rTime", e.releaseTime);
    e.releaseLevel = f ("rLevel", e.releaseLevel);
    return e;
}

#define BLOCK_FLOATS(X) X(pitchEgInt) X(vibRate) X(vibDepth) X(vibDelay) X(vibFade) X(vibOffset) X(mwToVib) X(atToVib) \
    X(cutoff) X(resonance) X(keyTrack) X(velToCutoff) X(filterEgInt) X(velToFilterEg) X(bendToCutoff) X(filterLfoRate) X(filterLfoDepth) \
    X(mwToCutoff) X(atToCutoff) X(velSens) X(velCurve) X(atToAmp)

std::unique_ptr<juce::XmlElement> UserProgram::toXml() const
{
    auto e = std::make_unique<juce::XmlElement> ("User");
    e->setAttribute ("name", name);
    if (kronosSource.isNotEmpty()) { e->setAttribute ("kronosPcg", kronosPcg); e->setAttribute ("kronosSource", kronosSource); }
    e->setAttribute ("voiceMode", voiceMode);
    e->setAttribute ("priority", priority);
    e->setAttribute ("portaOn", portaOn);
    e->setAttribute ("portamento", (double) portamento);
    e->setAttribute ("eqLow", (double) eqLowDb);
    e->setAttribute ("send1", (double) send1);
    e->setAttribute ("send2", (double) send2);
    e->setAttribute ("eqMid", (double) eqMidDb);
    e->setAttribute ("eqMidFreq", (double) eqMidFreq);
    e->setAttribute ("eqHigh", (double) eqHighDb);
    e->setAttribute ("portaFingered", portaFingered);
    e->setAttribute ("portaConstRate", portaConstRate);
    e->setAttribute ("bendUp", bendUp);
    e->setAttribute ("bendDown", bendDown);
    e->setAttribute ("fxType", fxType);
    e->setAttribute ("fxRate", (double) fxRate);
    e->setAttribute ("fxDepth", (double) fxDepth);
    e->setAttribute ("fxMix", (double) fxMix);

    for (int b = 0; b < kNumBlocks; ++b)
    {
        const auto& sb = block[(size_t) b];
        auto* x = e->createNewChildElement ("Block");
        x->setAttribute ("index", b);
        x->setAttribute ("filterType", sb.filterType);
        x->setAttribute ("monoLow", sb.monoLow);
        x->setAttribute ("monoHigh", sb.monoHigh);
        x->setAttribute ("articOn", sb.articOn);
        x->setAttribute ("articMask", sb.articMask);
        x->setAttribute ("articDir", sb.articDir);
       #define WRITE_F(n) x->setAttribute (#n, (double) sb.n);
        BLOCK_FLOATS (WRITE_F)
       #undef WRITE_F
        envToXml (sb.pitchEg, *x->createNewChildElement ("PitchEG"));
        envToXml (sb.filterEg, *x->createNewChildElement ("FilterEG"));
        envToXml (sb.ampEg, *x->createNewChildElement ("AmpEG"));
    }

    for (int i = 0; i < kNumOsc; ++i)
    {
        const auto& o = osc[(size_t) i];
        if (! o.on && o.instrument.isEmpty())
            continue;
        auto* x = e->createNewChildElement ("Osc");
        x->setAttribute ("index", i);
        x->setAttribute ("on", o.on);
        x->setAttribute ("instrument", o.instrument);
        x->setAttribute ("level", (double) o.levelDb);
        x->setAttribute ("pan", (double) o.pan);
        x->setAttribute ("transpose", o.transpose);
        x->setAttribute ("fine", (double) o.fineCents);
        x->setAttribute ("keyLow", o.keyLow);
        x->setAttribute ("keyHigh", o.keyHigh);
        x->setAttribute ("velLow", o.velLow);
        x->setAttribute ("velHigh", o.velHigh);
        x->setAttribute ("fadeIn", o.fadeIn);
        if (std::abs (o.velRampDb) > 0.01f) x->setAttribute ("velRamp", (double) o.velRampDb);
        x->setAttribute ("fadeOut", o.fadeOut);
        x->setAttribute ("fadeCurve", o.fadeCurve);
        x->setAttribute ("block", o.block);
        x->setAttribute ("keyOff", o.keyOff);
        x->setAttribute ("eqLow", (double) o.eqLowDb);
        x->setAttribute ("eqMid", (double) o.eqMidDb);
        x->setAttribute ("eqMidFreq", (double) o.eqMidFreq);
        x->setAttribute ("eqHigh", (double) o.eqHighDb);
    }
    return e;
}

UserProgram UserProgram::fromXml (const juce::XmlElement& e)
{
    UserProgram u;
    auto f = [&e] (const char* n, float def) { return (float) e.getDoubleAttribute (n, def); };
    u.name = e.getStringAttribute ("name", u.name);
    u.kronosPcg = e.getStringAttribute ("kronosPcg");
    u.kronosSource = e.getStringAttribute ("kronosSource");
    u.fxType = e.getIntAttribute ("fxType", 0);
    u.fxRate = f ("fxRate", u.fxRate);
    u.fxDepth = f ("fxDepth", u.fxDepth);
    u.fxMix = f ("fxMix", u.fxMix);
    u.eqLowDb = f ("eqLow", 0.0f);
    u.send1 = f ("send1", 1.0f);
    u.send2 = f ("send2", 1.0f);
    u.eqMidDb = f ("eqMid", 0.0f);
    u.eqMidFreq = f ("eqMidFreq", 1000.0f);
    u.eqHighDb = f ("eqHigh", 0.0f);

    if (e.getChildByName ("Block") != nullptr)
    {
        u.voiceMode = juce::jlimit (0, 2, e.getIntAttribute ("voiceMode", 0));
        u.priority = juce::jlimit (0, 2, e.getIntAttribute ("priority", 0));
        u.portaOn = e.getBoolAttribute ("portaOn", false);
        u.portamento = f ("portamento", u.portamento);
        u.portaFingered = e.getBoolAttribute ("portaFingered", false);
        u.portaConstRate = e.getBoolAttribute ("portaConstRate", false);
        u.bendUp = e.getIntAttribute ("bendUp", 2);
        u.bendDown = e.getIntAttribute ("bendDown", 2);

        for (auto* x : e.getChildWithTagNameIterator ("Block"))
        {
            const int b = x->getIntAttribute ("index", -1);
            if (b < 0 || b >= kNumBlocks) continue;
            auto& sb = u.block[(size_t) b];
            sb.filterType = juce::jlimit (0, FilterType::Num - 1, x->getIntAttribute ("filterType", sb.filterType));
            sb.monoLow = juce::jlimit (0, 127, x->getIntAttribute ("monoLow", 0));
            sb.monoHigh = juce::jlimit (0, 127, x->getIntAttribute ("monoHigh", 127));
            sb.articOn = x->getBoolAttribute ("articOn", false);
            sb.articMask = x->getIntAttribute ("articMask", 0);
            sb.articDir = juce::jlimit (0, 2, x->getIntAttribute ("articDir", 0));
           #define READ_F(n) sb.n = (float) x->getDoubleAttribute (#n, sb.n);
            BLOCK_FLOATS (READ_F)
           #undef READ_F
            sb.pitchEg = envFromXml (x->getChildByName ("PitchEG"), sb.pitchEg);
            sb.filterEg = envFromXml (x->getChildByName ("FilterEG"), sb.filterEg);
            sb.ampEg = envFromXml (x->getChildByName ("AmpEG"), sb.ampEg);
        }
    }
    else
    {
        // user iz prethodne verzije (jedan ADSR i filter)
        const float a = f ("attack", 0.001f), d = f ("decay", 0.5f), s = f ("sustain", 1.0f), r = f ("release", 0.3f);
        for (auto& sb : u.block)
        {
            sb.ampEg = { 0.0f, a, 1.0f, d, s, 0.01f, s, r, 0.0f };
            sb.cutoff = f ("cutoff", 20000.0f);
            sb.resonance = f ("resonance", 0.0f);
            sb.filterType = FilterType::LP12;
            sb.velSens = f ("velSens", 0.8f);
        }
        u.voiceMode = e.getBoolAttribute ("mono", false) ? VoiceMode::Mono : VoiceMode::Poly;
        const float porta = f ("portamento", 0.0f);
        u.portaOn = porta > 0.001f;
        if (u.portaOn) u.portamento = porta;
        u.bendUp = u.bendDown = e.getIntAttribute ("bendRange", 2);
    }

    // Starije verzije: 8 oscilatora sa oznakom bloka A/B. Sada: 0..7 = OSC1, 8..15 = OSC2.
    std::array<bool, kNumOsc> taken {};
    for (auto* x : e.getChildWithTagNameIterator ("Osc"))
    {
        int i = x->getIntAttribute ("index", -1);
        const int blk = juce::jlimit (0, kNumBlocks - 1, x->getIntAttribute ("block", i / kMsPerOsc));
        if (i < 0 || i >= kNumOsc) continue;
        if (i / kMsPerOsc != blk || taken[(size_t) i])
        {
            const bool used = x->getBoolAttribute ("on", false) || x->getStringAttribute ("instrument").isNotEmpty();
            if (! used) continue;
            i = -1;
            for (int k = blk * kMsPerOsc; k < (blk + 1) * kMsPerOsc; ++k)
                if (! taken[(size_t) k]) { i = k; break; }
            if (i < 0) continue;
        }
        taken[(size_t) i] = true;
        auto& o = u.osc[(size_t) i];
        o.on = x->getBoolAttribute ("on", false);
        o.instrument = x->getStringAttribute ("instrument");
        o.levelDb = (float) x->getDoubleAttribute ("level", 0.0);
        o.pan = (float) x->getDoubleAttribute ("pan", 0.0);
        o.transpose = x->getIntAttribute ("transpose", 0);
        o.fineCents = (float) x->getDoubleAttribute ("fine", 0.0);
        o.keyLow = juce::jlimit (0, 127, x->getIntAttribute ("keyLow", 0));
        o.keyHigh = juce::jlimit (0, 127, x->getIntAttribute ("keyHigh", 127));
        o.velLow = juce::jlimit (1, 127, x->getIntAttribute ("velLow", 1));
        o.velHigh = juce::jlimit (1, 127, x->getIntAttribute ("velHigh", 127));
        o.fadeIn = juce::jlimit (0, 127, x->getIntAttribute ("fadeIn", 0));
        o.velRampDb = (float) juce::jlimit (-40.0, 0.0, x->getDoubleAttribute ("velRamp", 0.0));
        o.fadeOut = juce::jlimit (0, 127, x->getIntAttribute ("fadeOut", 0));
        o.fadeCurve = x->getIntAttribute ("fadeCurve", 0);
        o.keyOff = x->getBoolAttribute ("keyOff", false);
        o.eqLowDb = (float) x->getDoubleAttribute ("eqLow", 0.0);
        o.eqMidDb = (float) x->getDoubleAttribute ("eqMid", 0.0);
        o.eqMidFreq = (float) x->getDoubleAttribute ("eqMidFreq", 1000.0);
        o.eqHighDb = (float) x->getDoubleAttribute ("eqHigh", 0.0);
    }
    for (int i = 0; i < kNumOsc; ++i)
        u.osc[(size_t) i].block = i / kMsPerOsc;
    return u;
}

juce::StringArray UserProgram::usedInstruments() const
{
    juce::StringArray s;
    for (auto& o : osc)
        if (o.on && o.instrument.isNotEmpty())
            s.addIfNotAlreadyThere (o.instrument);
    return s;
}

//==============================================================================
//  Performans
//==============================================================================
static void masterToXml (const MasterSettings& m, juce::XmlElement& e)
{
    e.setAttribute ("revSize", (double) m.reverbSize);
    e.setAttribute ("revDamp", (double) m.reverbDamping);
    e.setAttribute ("revLevel", (double) m.reverbLevel);
    e.setAttribute ("revDecay", (double) m.reverbDecay);
    e.setAttribute ("revPre", (double) m.reverbPreDelayMs);
    e.setAttribute ("revLowCut", (double) m.reverbLowCut);
    e.setAttribute ("revHighCut", (double) m.reverbHighCut);
    e.setAttribute ("revDiff", (double) m.reverbDiffusion);
    e.setAttribute ("revMod", (double) m.reverbMod);
    e.setAttribute ("revWidth", (double) m.reverbWidth);
    e.setAttribute ("dlyTime", (double) m.delayTimeMs);
    e.setAttribute ("dlyFb", (double) m.delayFeedback);
    e.setAttribute ("dlyLevel", (double) m.delayLevel);
    e.setAttribute ("eqLow", (double) m.eqLowDb);
    e.setAttribute ("eqMid", (double) m.eqMidDb);
    e.setAttribute ("eqHigh", (double) m.eqHighDb);
    e.setAttribute ("compThr", (double) m.compThresholdDb);
    e.setAttribute ("compRatio", (double) m.compRatio);
    e.setAttribute ("volume", (double) m.masterDb);
}

static MasterSettings masterFromXml (const juce::XmlElement& e)
{
    MasterSettings m;
    auto f = [&e] (const char* n, float def) { return (float) e.getDoubleAttribute (n, def); };
    m.reverbSize = f ("revSize", m.reverbSize);
    m.reverbDamping = f ("revDamp", m.reverbDamping);
    m.reverbLevel = f ("revLevel", m.reverbLevel);
    m.reverbDecay = f ("revDecay", m.reverbDecay);
    m.reverbPreDelayMs = f ("revPre", m.reverbPreDelayMs);
    m.reverbLowCut = f ("revLowCut", m.reverbLowCut);
    m.reverbHighCut = f ("revHighCut", m.reverbHighCut);
    m.reverbDiffusion = f ("revDiff", m.reverbDiffusion);
    m.reverbMod = f ("revMod", m.reverbMod);
    m.reverbWidth = f ("revWidth", m.reverbWidth);
    m.delayTimeMs = f ("dlyTime", m.delayTimeMs);
    m.delayFeedback = f ("dlyFb", m.delayFeedback);
    m.delayLevel = f ("dlyLevel", m.delayLevel);
    m.eqLowDb = f ("eqLow", 0.0f);
    m.eqMidDb = f ("eqMid", 0.0f);
    m.eqHighDb = f ("eqHigh", 0.0f);
    m.compThresholdDb = f ("compThr", m.compThresholdDb);
    m.compRatio = f ("compRatio", 1.0f);
    m.masterDb = f ("volume", 0.0f);
    return m;
}

std::unique_ptr<juce::XmlElement> Performance::toXml() const
{
    auto e = std::make_unique<juce::XmlElement> ("Performance");
    e->setAttribute ("name", name);
    e->setAttribute ("used", used);
    if (vectorOn) e->setAttribute ("vector", true);
    if (vectorEqualPower) e->setAttribute ("vectorEqualPower", true);
    if (category.isNotEmpty()) e->setAttribute ("categoryName", category);
    if (kronosSource.isNotEmpty()) e->setAttribute ("kronosSource", kronosSource);
    if (kronosPcg.isNotEmpty()) e->setAttribute ("kronosPcg", kronosPcg);
    masterToXml (master, *e->createNewChildElement ("Master"));

    for (int i = 0; i < kNumParts; ++i)
    {
        const auto& p = parts[(size_t) i];
        auto* x = e->createNewChildElement ("Part");
        x->setAttribute ("index", i);
        x->setAttribute ("on", p.on);
        x->setAttribute ("user", p.user);
        x->setAttribute ("volume", (double) p.volumeDb);
        x->setAttribute ("pan", (double) p.pan);
        x->setAttribute ("transpose", p.transpose);
        x->setAttribute ("octave", p.octave);
        x->setAttribute ("detune", (double) p.detuneCents);
        x->setAttribute ("eqLow", (double) p.eqLowDb);
        x->setAttribute ("eqMid", (double) p.eqMidDb);
        x->setAttribute ("eqHigh", (double) p.eqHighDb);
        x->setAttribute ("keyLow", p.keyLow);
        x->setAttribute ("keyHigh", p.keyHigh);
        x->setAttribute ("velLow", p.velLow);
        x->setAttribute ("velHigh", p.velHigh);
        x->setAttribute ("channel", p.midiChannel);
        x->setAttribute ("reverb", (double) p.reverbSend);
        x->setAttribute ("delay", (double) p.delaySend);
        x->setAttribute ("terca", p.terca);
        x->setAttribute ("sustain", p.sustainPedal);
        x->setAttribute ("expression", p.expression);
        if (p.vectorDir != 0) { x->setAttribute ("vjs", p.vectorDir); x->setAttribute ("vjsCenter", (double) p.vectorCenter); }
    }
    return e;
}

Performance Performance::fromXml (const juce::XmlElement& e)
{
    Performance perf;
    perf.name = e.getStringAttribute ("name", perf.name);
    perf.used = e.getBoolAttribute ("used", true);
    perf.vectorOn = e.getBoolAttribute ("vector", false);
    perf.vectorEqualPower = e.getBoolAttribute ("vectorEqualPower", false);
    perf.category = e.getStringAttribute ("categoryName");
    perf.kronosSource = e.getStringAttribute ("kronosSource");
    perf.kronosPcg = e.getStringAttribute ("kronosPcg");
    if (auto* m = e.getChildByName ("Master"))
        perf.master = masterFromXml (*m);

    for (auto* x : e.getChildWithTagNameIterator ("Part"))
    {
        const int i = x->getIntAttribute ("index", -1);
        if (i < 0 || i >= kNumParts) continue;
        auto& p = perf.parts[(size_t) i];
        p.on = x->getBoolAttribute ("on", false);
        p.user = x->getStringAttribute ("user");
        p.volumeDb = (float) x->getDoubleAttribute ("volume", 0.0);
        p.pan = (float) x->getDoubleAttribute ("pan", 0.0);
        p.transpose = x->getIntAttribute ("transpose", 0);
        p.octave = x->getIntAttribute ("octave", 0);
        p.detuneCents = (float) x->getDoubleAttribute ("detune", 0.0);
        p.eqLowDb = (float) x->getDoubleAttribute ("eqLow", 0.0);
        p.eqMidDb = (float) x->getDoubleAttribute ("eqMid", 0.0);
        p.eqHighDb = (float) x->getDoubleAttribute ("eqHigh", 0.0);
        p.keyLow = juce::jlimit (0, 127, x->getIntAttribute ("keyLow", 0));
        p.keyHigh = juce::jlimit (0, 127, x->getIntAttribute ("keyHigh", 127));
        p.velLow = juce::jlimit (1, 127, x->getIntAttribute ("velLow", 1));
        p.velHigh = juce::jlimit (1, 127, x->getIntAttribute ("velHigh", 127));
        p.midiChannel = juce::jlimit (0, 16, x->getIntAttribute ("channel", 0));
        p.reverbSend = (float) x->getDoubleAttribute ("reverb", 0.2);
        p.delaySend = (float) x->getDoubleAttribute ("delay", 0.0);
        p.terca = x->getBoolAttribute ("terca", false);
        p.sustainPedal = x->getBoolAttribute ("sustain", true);
        p.expression = x->getBoolAttribute ("expression", true);
        p.vectorDir = juce::jlimit (0, 4, x->getIntAttribute ("vjs", 0));
        p.vectorCenter = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("vjsCenter", 1.0));
    }
    return perf;
}

Performance Performance::makeDefault (const juce::String& firstUser)
{
    Performance p;
    p.parts[0].on = firstUser.isNotEmpty();
    p.parts[0].user = firstUser;
    return p;
}

//==============================================================================
//  Terce
//==============================================================================
juce::String LearnTrigger::describe() const
{
    if (type == CC)   return "CC " + juce::String (number) + (channel > 0 ? " (k" + juce::String (channel) + ")" : juce::String());
    if (type == Note) return juce::MidiMessage::getMidiNoteName (number, true, true, 3) + (channel > 0 ? " (k" + juce::String (channel) + ")" : juce::String());
    return T ("nije dodeljeno");
}

static int maskOf (std::initializer_list<int> intervals)
{
    int m = 0;
    for (int i : intervals) m |= 1 << (i % 12);
    return m;
}

TercaSettings TercaSettings::makeDefault()
{
    TercaSettings s;
    // Dur: diatonske terce, hromatski tonovi dobijaju najblizu "lepu" tercu
    s.scales.push_back ({ "Dur", { 4, 3, 3, 4, 3, 4, 3, 4, 4, 3, 4, 3 },
                                 { -3, -4, -3, -3, -4, -3, -4, -3, -4, -4, -3, -4 } });
    // Mol (harmonski, kao u nasoj muzici: E -> G# u A molu)
    s.scales.push_back ({ "Mol", { 3, 4, 3, 4, 3, 3, 3, 4, 4, 3, 4, 3 },
                                 { -4, -3, -3, -3, -4, -3, -4, -4, -3, -4, -3, -4 } });

    s.rules.push_back ({ maskOf ({ 0, 4, 7 }), 0, 0 });       // dur akord
    s.rules.push_back ({ maskOf ({ 0, 3, 7 }), 1, 0 });       // mol akord
    s.rules.push_back ({ maskOf ({ 0, 4, 7, 10 }), 0, 0 });   // dur sept
    s.rules.push_back ({ maskOf ({ 0, 3, 7, 10 }), 1, 0 });   // mol sept
    s.rules.push_back ({ maskOf ({ 0, 4 }), 0, 0 });          // dva tona: velika terca
    s.rules.push_back ({ maskOf ({ 0, 3 }), 1, 0 });          // dva tona: mala terca
    return s;
}

std::unique_ptr<juce::XmlElement> TercaSettings::toXml() const
{
    auto e = std::make_unique<juce::XmlElement> ("Terce");
    e->setAttribute ("voiceMode", voiceMode);
    e->setAttribute ("velocity", (double) velocity);
    e->setAttribute ("topNoteOnly", topNoteOnly);
    e->setAttribute ("learnType", learn.type);
    e->setAttribute ("learnChannel", learn.channel);
    e->setAttribute ("learnNumber", learn.number);

    for (auto& sc : scales)
    {
        auto* x = e->createNewChildElement ("Scale");
        x->setAttribute ("name", sc.name);
        juce::StringArray a, b;
        for (int i = 0; i < 12; ++i) { a.add (juce::String (sc.above[(size_t) i])); b.add (juce::String (sc.below[(size_t) i])); }
        x->setAttribute ("above", a.joinIntoString (" "));
        x->setAttribute ("below", b.joinIntoString (" "));
    }
    for (auto& r : rules)
    {
        auto* x = e->createNewChildElement ("Rule");
        x->setAttribute ("mask", r.mask);
        x->setAttribute ("scale", r.scale);
        x->setAttribute ("rootShift", r.rootShift);
    }
    return e;
}

TercaSettings TercaSettings::fromXml (const juce::XmlElement& e)
{
    TercaSettings s;
    s.voiceMode = juce::jlimit (0, 2, e.getIntAttribute ("voiceMode", 0));
    s.velocity = (float) e.getDoubleAttribute ("velocity", 0.8);
    s.topNoteOnly = e.getBoolAttribute ("topNoteOnly", true);
    s.learn.type = e.getIntAttribute ("learnType", 0);
    s.learn.channel = e.getIntAttribute ("learnChannel", 0);
    s.learn.number = e.getIntAttribute ("learnNumber", 0);

    for (auto* x : e.getChildWithTagNameIterator ("Scale"))
    {
        TercaScale sc;
        sc.name = x->getStringAttribute ("name", "Skala");
        auto a = juce::StringArray::fromTokens (x->getStringAttribute ("above"), " ", {});
        auto b = juce::StringArray::fromTokens (x->getStringAttribute ("below"), " ", {});
        for (int i = 0; i < 12; ++i)
        {
            sc.above[(size_t) i] = juce::jlimit (-24, 24, a[i].getIntValue());
            sc.below[(size_t) i] = juce::jlimit (-24, 24, b[i].getIntValue());
        }
        s.scales.push_back (sc);
    }
    for (auto* x : e.getChildWithTagNameIterator ("Rule"))
        s.rules.push_back ({ x->getIntAttribute ("mask") & 0xfff, x->getIntAttribute ("scale"), x->getIntAttribute ("rootShift") });

    if (s.scales.empty())
    {
        auto d = makeDefault();
        s.scales = d.scales;
        if (s.rules.empty()) s.rules = d.rules;
    }
    return s;
}

namespace Terca
{
    static int rotateDown (int mask, int r)
    {
        // tonovi relativno od r: bit (pc - r) mod 12
        return ((mask >> r) | (mask << (12 - r))) & 0xfff;
    }

    bool recognize (const TercaSettings& s, int pcMask, int& root, int& scale)
    {
        pcMask &= 0xfff;
        if (pcMask == 0)
            return false;

        for (auto& rule : s.rules)
        {
            if (rule.mask == 0 || rule.scale < 0 || rule.scale >= (int) s.scales.size())
                continue;
            for (int r = 0; r < 12; ++r)
            {
                if (! (pcMask & (1 << r)))
                    continue;   // tonika mora da bude odsvirana
                if (rotateDown (pcMask, r) == rule.mask)
                {
                    root = ((r + rule.rootShift) % 12 + 12) % 12;
                    scale = rule.scale;
                    return true;
                }
            }
        }
        return false;
    }

    int harmonize (const TercaSettings& s, int root, int scale, int note, int out[2])
    {
        if (scale < 0 || scale >= (int) s.scales.size())
            return 0;
        const auto& sc = s.scales[(size_t) scale];
        const int pc = ((note - root) % 12 + 12) % 12;
        int n = 0;
        auto add = [&] (int interval)
        {
            const int h = note + interval;
            if (interval != 0 && h >= 0 && h <= 127)
                out[n++] = h;
        };
        if (s.voiceMode == 0 || s.voiceMode == 2) add (sc.above[(size_t) pc]);
        if (s.voiceMode == 1 || s.voiceMode == 2) add (sc.below[(size_t) pc]);
        return n;
    }

    juce::String pcName (int pc)
    {
        static const char* n[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };
        return n[((pc % 12) + 12) % 12];
    }

    juce::String maskToText (int mask)
    {
        static const char* deg[] = { "1", "b2", "2", "m3", "3", "4", "b5", "5", "#5", "6", "m7", "7" };
        juce::StringArray parts;
        for (int i = 0; i < 12; ++i)
            if (mask & (1 << i))
                parts.add (deg[i]);
        return parts.joinIntoString (" ");
    }

    juce::String keyName (int root, const TercaSettings& s, int scale)
    {
        return pcName (root) + " " + (scale >= 0 && scale < (int) s.scales.size() ? s.scales[(size_t) scale].name.toLowerCase() : juce::String ("?"));
    }
}

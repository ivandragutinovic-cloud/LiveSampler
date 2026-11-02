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

#define BLOCK_FLOATS(X) X(pitchEgInt) X(vibRate) X(vibDepth) X(vibDelay) X(mwToVib) X(atToVib) \
    X(cutoff) X(resonance) X(keyTrack) X(velToCutoff) X(filterEgInt) X(filterLfoRate) X(filterLfoDepth) \
    X(mwToCutoff) X(atToCutoff) X(velSens) X(velCurve) X(atToAmp)

std::unique_ptr<juce::XmlElement> UserProgram::toXml() const
{
    auto e = std::make_unique<juce::XmlElement> ("User");
    e->setAttribute ("name", name);
    e->setAttribute ("voiceMode", voiceMode);
    e->setAttribute ("priority", priority);
    e->setAttribute ("portaOn", portaOn);
    e->setAttribute ("portamento", (double) portamento);
    e->setAttribute ("portaFingered", portaFingered);
    e->setAttribute ("portaConstRate", portaConstRate);
    e->setAttribute ("bendUp", bendUp);
    e->setAttribute ("bendDown", bendDown);
    e->setAttribute ("fxType", fxType);
    e->setAttribute ("fxRate", (double) fxRate);
    e->setAttribute ("fxDepth", (double) fxDepth);
    e->setAttribute ("fxMix", (double) fxMix);

    for (int b = 0; b < 2; ++b)
    {
        const auto& sb = block[(size_t) b];
        auto* x = e->createNewChildElement ("Block");
        x->setAttribute ("index", b);
        x->setAttribute ("filterType", sb.filterType);
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
        x->setAttribute ("fadeOut", o.fadeOut);
        x->setAttribute ("fadeCurve", o.fadeCurve);
        x->setAttribute ("block", o.block);
    }
    return e;
}

UserProgram UserProgram::fromXml (const juce::XmlElement& e)
{
    UserProgram u;
    auto f = [&e] (const char* n, float def) { return (float) e.getDoubleAttribute (n, def); };
    u.name = e.getStringAttribute ("name", u.name);
    u.fxType = e.getIntAttribute ("fxType", 0);
    u.fxRate = f ("fxRate", u.fxRate);
    u.fxDepth = f ("fxDepth", u.fxDepth);
    u.fxMix = f ("fxMix", u.fxMix);

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
            if (b < 0 || b > 1) continue;
            auto& sb = u.block[(size_t) b];
            sb.filterType = juce::jlimit (0, FilterType::Num - 1, x->getIntAttribute ("filterType", sb.filterType));
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

    for (auto* x : e.getChildWithTagNameIterator ("Osc"))
    {
        const int i = x->getIntAttribute ("index", -1);
        if (i < 0 || i >= kNumOsc) continue;
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
        o.fadeOut = juce::jlimit (0, 127, x->getIntAttribute ("fadeOut", 0));
        o.fadeCurve = x->getIntAttribute ("fadeCurve", 0);
        o.block = juce::jlimit (0, 1, x->getIntAttribute ("block", 0));
    }
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
        x->setAttribute ("keyLow", p.keyLow);
        x->setAttribute ("keyHigh", p.keyHigh);
        x->setAttribute ("velLow", p.velLow);
        x->setAttribute ("velHigh", p.velHigh);
        x->setAttribute ("channel", p.midiChannel);
        x->setAttribute ("reverb", (double) p.reverbSend);
        x->setAttribute ("delay", (double) p.delaySend);
        x->setAttribute ("terca", p.terca);
        x->setAttribute ("sustain", p.sustainPedal);
    }
    return e;
}

Performance Performance::fromXml (const juce::XmlElement& e)
{
    Performance perf;
    perf.name = e.getStringAttribute ("name", perf.name);
    perf.used = e.getBoolAttribute ("used", true);
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
        p.keyLow = juce::jlimit (0, 127, x->getIntAttribute ("keyLow", 0));
        p.keyHigh = juce::jlimit (0, 127, x->getIntAttribute ("keyHigh", 127));
        p.velLow = juce::jlimit (1, 127, x->getIntAttribute ("velLow", 1));
        p.velHigh = juce::jlimit (1, 127, x->getIntAttribute ("velHigh", 127));
        p.midiChannel = juce::jlimit (0, 16, x->getIntAttribute ("channel", 0));
        p.reverbSend = (float) x->getDoubleAttribute ("reverb", 0.2);
        p.delaySend = (float) x->getDoubleAttribute ("delay", 0.0);
        p.terca = x->getBoolAttribute ("terca", false);
        p.sustainPedal = x->getBoolAttribute ("sustain", true);
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

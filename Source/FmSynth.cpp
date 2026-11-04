#include "FmSynth.h"
#include "Mod7Table.h"
#include <map>
#include <set>

namespace Fm
{
    namespace
    {
        // DX7 algoritmi: (operator, njegov modulator) i nosioci. Operator n je OSC (7-n) u MOD-7.
        struct DxAlg { std::vector<std::pair<int, int>> mods; std::vector<int> carriers; };
        const DxAlg& dxAlg (int a)
        {
            static const DxAlg t[33] = {
                {},
                { { {1,2},{3,4},{4,5},{5,6} }, {1,3} }, { { {1,2},{3,4},{4,5},{5,6} }, {1,3} },
                { { {1,2},{2,3},{4,5},{5,6} }, {1,4} }, { { {1,2},{2,3},{4,5},{5,6} }, {1,4} },
                { { {1,2},{3,4},{5,6} }, {1,3,5} }, { { {1,2},{3,4},{5,6} }, {1,3,5} },
                { { {1,2},{3,4},{3,5},{5,6} }, {1,3} }, { { {1,2},{3,4},{3,5},{5,6} }, {1,3} }, { { {1,2},{3,4},{3,5},{5,6} }, {1,3} },
                { { {1,2},{2,3},{4,5},{4,6} }, {1,4} }, { { {1,2},{2,3},{4,5},{4,6} }, {1,4} },
                { { {1,2},{3,4},{3,5},{3,6} }, {1,3} }, { { {1,2},{3,4},{3,5},{3,6} }, {1,3} },
                { { {1,2},{3,4},{4,5},{4,6} }, {1,3} }, { { {1,2},{3,4},{4,5},{4,6} }, {1,3} },
                { { {1,2},{1,3},{3,4},{1,5},{5,6} }, {1} }, { { {1,2},{1,3},{3,4},{1,5},{5,6} }, {1} },
                { { {1,2},{1,3},{1,4},{4,5},{5,6} }, {1} },
                { { {1,2},{2,3},{4,6},{5,6} }, {1,4,5} },
                { { {1,3},{2,3},{4,5},{4,6} }, {1,2,4} },
                { { {1,3},{2,3},{4,6},{5,6} }, {1,2,4,5} },
                { { {1,2},{3,6},{4,6},{5,6} }, {1,3,4,5} },
                { { {2,3},{4,6},{5,6} }, {1,2,4,5} },
                { { {3,6},{4,6},{5,6} }, {1,2,3,4,5} },
                { { {4,6},{5,6} }, {1,2,3,4,5} },
                { { {2,3},{4,5},{4,6} }, {1,2,4} }, { { {2,3},{4,5},{4,6} }, {1,2,4} },
                { { {1,2},{3,4},{4,5} }, {1,3,6} },
                { { {3,4},{5,6} }, {1,2,3,5} },
                { { {3,4},{4,5} }, {1,2,3,6} },
                { { {5,6} }, {1,2,3,4,5} },
                { {}, {1,2,3,4,5,6} } };
            return t[juce::jlimit (0, 32, a)];
        }

        const std::map<std::string, const Mod7Table::Field*>& fieldMap()
        {
            static std::map<std::string, const Mod7Table::Field*> m = []
            {
                std::map<std::string, const Mod7Table::Field*> r;
                for (int i = 0; i < Mod7Table::numFields; ++i)
                    r.emplace (Mod7Table::fields[i].name, &Mod7Table::fields[i]);   // prvo pojavljivanje
                return r;
            }();
            return m;
        }

        int readField (const juce::uint8* b, const char* name)
        {
            auto it = fieldMap().find (name);
            if (it == fieldMap().end()) { jassertfalse; return 0; }
            const auto& f = *it->second;
            juce::uint32 v = 0;
            int n = 0;
            for (int s = 0; s < f.numSegs; ++s)
            {
                const auto& g = f.segs[s];
                const int w = g.hi - g.lo + 1;
                v |= (juce::uint32) ((b[g.byte] >> g.lo) & ((1 << w) - 1)) << n;
                n += w;
            }
            if (f.signedField && n > 0 && (v & (1u << (n - 1))) != 0)
                return (int) v - (1 << n);
            return (int) v;
        }

        float kTime (int v) { return v <= 0 ? 0.001f : 0.003f * std::exp (0.09f * (float) v); }
        float kHz (int v) { return 20.0f * std::pow (1000.0f, (float) juce::jlimit (0, 99, v) / 99.0f); }

        EnvSettings readEg (const juce::uint8* b, const juce::String& p, float levelScale)
        {
            auto r = [&] (const char* n) { return readField (b, (p + n).toRawUTF8()); };
            EnvSettings e;
            e.start = (float) r ("Start Level") / levelScale;
            e.attackLevel = (float) r ("Attack Level") / levelScale;
            e.breakLevel = (float) r ("Break Level") / levelScale;
            e.sustain = (float) r ("Sustain Level") / levelScale;
            e.releaseLevel = (float) r ("Release Level") / levelScale;
            e.attackTime = kTime (r ("Attack Time"));
            e.decayTime = kTime (r ("Decay Time"));
            e.slopeTime = kTime (r ("Slope Time"));
            e.releaseTime = kTime (r ("Release Time"));
            return e;
        }

        float velFactor (float sens, float vel01)
        {
            return sens >= 0.0f ? 1.0f - sens * (1.0f - vel01) : 1.0f + sens * vel01;
        }
    }

    //==========================================================================
    Patch Patch::fromMod7 (const juce::uint8* b, const juce::uint8* program, int exiSlot)
    {
        Patch p;
        auto r = [b] (const juce::String& n) { return readField (b, n.toRawUTF8()); };
        for (int k = 0; k < 6; ++k)
        {
            auto& o = p.op[k];
            const juce::String O = "[OSC" + juce::String (k + 1) + "] ";
            o.on = r (O + "OSC On/Off") != 0;
            o.mode = r (O + "OSC Mode");
            if (o.mode == 0) o.on = false;
            o.ratio = (double) r (O + "Frequency Ratio (Coarse)") + (double) r (O + "Frequency Ratio (Fine)") / 1000.0;
            o.offsetHz = (double) r (O + "Frequency Offset (Coarse)") + (double) r (O + "Frequency Offset (Fine)") / 100.0;
            o.feedback = (float) r (O + "Feedback Level") / 100.0f;
            o.volume = (float) r (O + "Volume") / 100.0f;
            o.eg = r (O + "Volume EG Select");
            o.velSens = (float) r (O + "Volume Velocity Sens") / 100.0f;
            o.drive = (float) r (O + "Waveshaper Drive");
            const juce::String K = "[OSC" + juce::String (k + 1) + " Keyboard Track] ";
            o.ktLow = r (K + "Low Key");
            o.ktCenter = r (K + "Center Key");
            o.ktHigh = r (K + "High Key");
            o.ktRamp[0] = (float) r (K + "Low Ramp") / 100.0f;
            o.ktRamp[1] = (float) r (K + "Center Low Ramp") / 100.0f;
            o.ktRamp[2] = (float) r (K + "Center High Ramp") / 100.0f;
            o.ktRamp[3] = (float) r (K + "High Ramp") / 100.0f;
            // patch kablovi (ulazi)
            const juce::String J = "[Input Jack] OSC" + juce::String (k + 1);
            const int in1 = r (J + " In1");
            const int in2 = r (k == 4 ? juce::String ("[Input Jack] OSC5 B") : J + " In2");
            const float l1 = (float) r (O + "Input Level1") / 100.0f, l2 = (float) r (O + "Input Level2") / 100.0f;
            if (in1 > 0) o.inputs.push_back ({ in1, l1 });
            if (in2 > 0) o.inputs.push_back ({ in2, l2 });
        }
        for (int e = 0; e < 9; ++e)
            p.eg[e] = readEg (b, "[EG" + juce::String (e + 1) + "] ", 99.0f);
        p.ampEg = readEg (b, "[Amplifier EG] ", 99.0f);
        p.ampLevel = (float) r ("[Amplifier] Level") / 127.0f;
        p.ampVel = (float) r ("[Amplifier] Velocity Intensity") / 99.0f;

        for (int m = 0; m < 3; ++m)
        {
            const juce::String M = "[Mixer" + juce::String (m + 1) + "] ";
            p.mix[m].in[0] = r ("[Input Jack] Mixer" + juce::String (m + 1) + " In1");
            p.mix[m].in[1] = r ("[Input Jack] Mixer" + juce::String (m + 1) + " In2");
            p.mix[m].level[0] = (float) r (M + "Level1") / 100.0f * (r (M + "Level1 Phase Invert") ? -1.0f : 1.0f);
            p.mix[m].level[1] = (float) r (M + "Level2") / 100.0f * (r (M + "Level2 Phase Invert") ? -1.0f : 1.0f);
        }
        for (int c = 0; c < 6; ++c)
        {
            const juce::String n = juce::String (c + 1);
            p.out[c].in = r ("[Input Jack] Output Mixer" + n);
            p.out[c].level = (float) r ("[Output Mixer] Mixer" + n + " Level") / 100.0f * (r ("[Output Mixer] Mixer" + n + " Phase Invert") ? -1.0f : 1.0f);
            p.out[c].pan = juce::jlimit (-1.0f, 1.0f, ((float) r ("[Output Mixer] Mixer" + n + " Pan") - 64.0f) / 63.0f);
        }
        for (int f = 0; f < 2; ++f)
        {
            const juce::String F = f == 0 ? "[Filter A] " : "[Filter B] ";
            auto& fl = p.flt[f];
            fl.bypass = r (F + "Bypass") != 0;
            fl.type = r (F + "Filter Type");
            fl.cutoffHz = kHz (r (F + "Cutoff"));
            fl.reso = (float) r (F + "Resonance") / 99.0f;
            fl.in = r (f == 0 ? "[Input Jack] Filter A" : "[Input Jack] Filter B");
            fl.eg = r (F + "EG Select");
            fl.egInt = (float) r (F + "EG Intensity") / 99.0f;
        }
        p.noiseVol = (float) r ("[Noise Generator] Volume") / 100.0f;
        p.noiseEg = r ("[Noise Generator] Volume EG Select");

        // DX7 template algoritam: veze izmedju operatora i nosioci su u samom algoritmu
        const int alg = r ("[OSC] Algorithm");
        if (alg > 0)
        {
            // Template algoritam: veze nisu u kablovima. Spisak MOD-7 template-a nije dokumentovan, pa se DX7 algoritam
            // pronalazi po nosiocima (Macro: Carrier) i operatoru sa feedback-om; od vise mogucih bira se najblizi broju (template - 47).
            std::set<int> carriers;
            std::vector<int> fbOsc;
            for (int k = 0; k < 6; ++k)
            {
                if (r ("[Macro] OSC" + juce::String (k + 1) + " Class") == 0) carriers.insert (k);
                if (p.op[k].feedback > 0.0f) fbOsc.push_back (k);
            }
            static const int fbOp[33] = { 0, 6,2,6,6,6,6,6,4,2,3,6,2,6,6,2,6,2,3,6,3,3,6,6,6,6,6,3,5,6,5,6,6 };
            int best = 0, bestScore = -1, bestDist = 1000;
            for (int n = 1; n <= 32; ++n)
            {
                std::set<int> c;
                for (int op : dxAlg (n).carriers) c.insert (6 - op);
                int score = (c == carriers ? 2 : 0) + (fbOsc.size() == 1 && fbOsc[0] == 6 - fbOp[n] ? 1 : 0);
                const int dist = std::abs (n - (alg - 47));
                if (score > bestScore || (score == bestScore && dist < bestDist)) { best = n; bestScore = score; bestDist = dist; }
            }
            if (bestScore < 2) best = 0;   // nijedan DX algoritam nema te nosioce
            p.dxAlgorithm = best;
        }
        if (p.dxAlgorithm > 0)
            p.applyDxAlgorithm (p.dxAlgorithm);

        if (program != nullptr)
        {
            const int common = exiSlot == 0 ? 2857 : 3909;
            p.transpose = (int) (juce::int8) program[common + 1];
            p.patchGain = (float) program[common + 9] / 127.0f;   // Patch Level
        }
        bool anyOut = false;
        for (auto& o : p.out) anyOut = anyOut || (o.in > 0 && std::abs (o.level) > 0.0f);
        p.valid = anyOut;
        return p;
    }

    std::vector<int> dxCarriers (int a) { return dxAlg (a).carriers; }
    std::vector<std::pair<int, int>> dxModulations (int a) { return dxAlg (a).mods; }

    void Patch::applyDxAlgorithm (int n)
    {
        dxAlgorithm = juce::jlimit (1, 32, n);
        const auto& t = dxAlg (dxAlgorithm);
        auto osc = [] (int opNum) { return 6 - opNum; };   // OP1 = OSC6 ... OP6 = OSC1
        for (auto& o : op) o.inputs.erase (std::remove_if (o.inputs.begin(), o.inputs.end(),
                                                           [] (const std::pair<int, float>& x) { return x.first >= Osc1 && x.first < Osc1 + 6; }), o.inputs.end());
        for (auto& m : t.mods)
            op[osc (m.first)].inputs.push_back ({ Osc1 + osc (m.second), 1.0f });
        // nosioci idu na izlaz; postojeci kablovi ka izlazu (npr. filteri) ostaju
        std::vector<Out> outs;
        for (int c : t.carriers) outs.push_back ({ Osc1 + osc (c), 1.0f, 0.0f });
        for (auto& o : out) if (o.in > 0 && (o.in < Osc1 || o.in >= Osc1 + 6)) outs.push_back (o);
        for (int c = 0; c < 6; ++c) out[c] = c < (int) outs.size() ? outs[(size_t) c] : Out { 0, 0.0f, 0.0f };
        valid = true;
    }

    Patch Patch::makeInit()
    {
        Patch p;
        for (int k = 0; k < 6; ++k)
        {
            auto& o = p.op[k];
            o.on = true;
            o.mode = 1;
            o.ratio = 1.0;
            o.volume = 0.0f;
            o.eg = k + 1;
        }
        for (auto& e : p.eg) { e = EnvSettings(); e.start = 0.0f; e.attackTime = 0.003f; e.attackLevel = 1.0f; e.decayTime = 1.5f; e.breakLevel = 0.7f; e.slopeTime = 3.0f; e.sustain = 0.5f; e.releaseTime = 0.4f; e.releaseLevel = 0.0f; }
        p.ampEg = EnvSettings();
        p.ampEg.start = 0.0f; p.ampEg.attackTime = 0.002f; p.ampEg.attackLevel = 1.0f; p.ampEg.decayTime = 0.5f; p.ampEg.breakLevel = 1.0f;
        p.ampEg.slopeTime = 0.5f; p.ampEg.sustain = 1.0f; p.ampEg.releaseTime = 0.4f;
        p.op[opIndex (1)].volume = 0.9f;    // OP1 nosilac
        p.op[opIndex (2)].volume = 0.35f;   // OP2 modulator (malo svetliji ton)
        p.op[opIndex (2)].velSens = 0.5f;
        p.ampVel = 0.6f;
        p.applyDxAlgorithm (1);
        return p;
    }

    std::unique_ptr<juce::XmlElement> Patch::toXml() const
    {
        auto x = std::make_unique<juce::XmlElement> ("FmPatch");
        x->setAttribute ("dx", dxAlgorithm);
        x->setAttribute ("transpose", transpose);
        x->setAttribute ("gain", (double) patchGain);
        x->setAttribute ("ampLevel", (double) ampLevel);
        x->setAttribute ("ampVel", (double) ampVel);
        x->setAttribute ("noise", (double) noiseVol);
        x->setAttribute ("noiseEg", noiseEg);
        auto envXml = [] (juce::XmlElement& e, const EnvSettings& s)
        {
            e.setAttribute ("s", (double) s.start); e.setAttribute ("al", (double) s.attackLevel); e.setAttribute ("bl", (double) s.breakLevel);
            e.setAttribute ("sl", (double) s.sustain); e.setAttribute ("rl", (double) s.releaseLevel);
            e.setAttribute ("at", (double) s.attackTime); e.setAttribute ("dt", (double) s.decayTime);
            e.setAttribute ("st", (double) s.slopeTime); e.setAttribute ("rt", (double) s.releaseTime);
        };
        for (int k = 0; k < 6; ++k)
        {
            const auto& o = op[k];
            auto* e = x->createNewChildElement ("Op");
            e->setAttribute ("on", o.on); e->setAttribute ("mode", o.mode);
            e->setAttribute ("ratio", o.ratio); e->setAttribute ("hz", o.offsetHz);
            e->setAttribute ("fb", (double) o.feedback); e->setAttribute ("vol", (double) o.volume);
            e->setAttribute ("eg", o.eg); e->setAttribute ("vel", (double) o.velSens);
            e->setAttribute ("ktLo", o.ktLow); e->setAttribute ("ktC", o.ktCenter); e->setAttribute ("ktHi", o.ktHigh);
            juce::StringArray kr, in;
            for (auto v : o.ktRamp) kr.add (juce::String (v, 3));
            for (auto& i : o.inputs) in.add (juce::String (i.first) + ":" + juce::String (i.second, 3));
            e->setAttribute ("ktRamp", kr.joinIntoString (" "));
            e->setAttribute ("in", in.joinIntoString (" "));
            e->setAttribute ("drive", (double) o.drive);
        }
        for (int i = 0; i < 9; ++i) { auto* e = x->createNewChildElement ("Eg"); e->setAttribute ("i", i); envXml (*e, eg[i]); }
        { auto* e = x->createNewChildElement ("AmpEg"); envXml (*e, ampEg); }
        for (int m = 0; m < 3; ++m)
        {
            auto* e = x->createNewChildElement ("Mix");
            e->setAttribute ("in1", mix[m].in[0]); e->setAttribute ("in2", mix[m].in[1]);
            e->setAttribute ("l1", (double) mix[m].level[0]); e->setAttribute ("l2", (double) mix[m].level[1]);
        }
        for (auto& o : out) { auto* e = x->createNewChildElement ("Out"); e->setAttribute ("in", o.in); e->setAttribute ("level", (double) o.level); e->setAttribute ("pan", (double) o.pan); }
        for (auto& f : flt)
        {
            auto* e = x->createNewChildElement ("Filter");
            e->setAttribute ("bypass", f.bypass); e->setAttribute ("type", f.type); e->setAttribute ("hz", (double) f.cutoffHz);
            e->setAttribute ("reso", (double) f.reso); e->setAttribute ("in", f.in); e->setAttribute ("eg", f.eg); e->setAttribute ("egInt", (double) f.egInt);
        }
        return x;
    }

    Patch Patch::fromXml (const juce::XmlElement& x)
    {
        Patch p;
        p.dxAlgorithm = x.getIntAttribute ("dx", 0);
        p.transpose = x.getIntAttribute ("transpose", 0);
        p.patchGain = (float) x.getDoubleAttribute ("gain", 1.0);
        p.ampLevel = (float) x.getDoubleAttribute ("ampLevel", 1.0);
        p.ampVel = (float) x.getDoubleAttribute ("ampVel", 0.0);
        p.noiseVol = (float) x.getDoubleAttribute ("noise", 0.0);
        p.noiseEg = x.getIntAttribute ("noiseEg", 0);
        auto env = [] (const juce::XmlElement& e)
        {
            EnvSettings s;
            s.start = (float) e.getDoubleAttribute ("s"); s.attackLevel = (float) e.getDoubleAttribute ("al", 1.0); s.breakLevel = (float) e.getDoubleAttribute ("bl", 1.0);
            s.sustain = (float) e.getDoubleAttribute ("sl", 1.0); s.releaseLevel = (float) e.getDoubleAttribute ("rl");
            s.attackTime = (float) e.getDoubleAttribute ("at", 0.001); s.decayTime = (float) e.getDoubleAttribute ("dt", 0.3);
            s.slopeTime = (float) e.getDoubleAttribute ("st", 0.3); s.releaseTime = (float) e.getDoubleAttribute ("rt", 0.3);
            return s;
        };
        int k = 0;
        for (auto* e : x.getChildWithTagNameIterator ("Op"))
        {
            if (k >= 6) break;
            auto& o = p.op[k++];
            o.on = e->getBoolAttribute ("on"); o.mode = e->getIntAttribute ("mode", 1);
            o.ratio = e->getDoubleAttribute ("ratio", 1.0); o.offsetHz = e->getDoubleAttribute ("hz");
            o.feedback = (float) e->getDoubleAttribute ("fb"); o.volume = (float) e->getDoubleAttribute ("vol");
            o.eg = e->getIntAttribute ("eg"); o.velSens = (float) e->getDoubleAttribute ("vel");
            o.ktLow = e->getIntAttribute ("ktLo", 0); o.ktCenter = e->getIntAttribute ("ktC", 60); o.ktHigh = e->getIntAttribute ("ktHi", 127);
            const auto kr = juce::StringArray::fromTokens (e->getStringAttribute ("ktRamp"), " ", "");
            for (int r = 0; r < 4 && r < kr.size(); ++r) o.ktRamp[r] = kr[r].getFloatValue();
            for (auto& t : juce::StringArray::fromTokens (e->getStringAttribute ("in"), " ", ""))
                if (t.containsChar (':')) o.inputs.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(), t.fromFirstOccurrenceOf (":", false, false).getFloatValue() });
            o.drive = (float) e->getDoubleAttribute ("drive");
        }
        for (auto* e : x.getChildWithTagNameIterator ("Eg"))
        {
            const int i = e->getIntAttribute ("i", -1);
            if (i >= 0 && i < 9) p.eg[i] = env (*e);
        }
        if (auto* e = x.getChildByName ("AmpEg")) p.ampEg = env (*e);
        int m = 0;
        for (auto* e : x.getChildWithTagNameIterator ("Mix"))
        {
            if (m >= 3) break;
            p.mix[m].in[0] = e->getIntAttribute ("in1"); p.mix[m].in[1] = e->getIntAttribute ("in2");
            p.mix[m].level[0] = (float) e->getDoubleAttribute ("l1", 1.0); p.mix[m].level[1] = (float) e->getDoubleAttribute ("l2", 1.0);
            ++m;
        }
        int c = 0;
        for (auto* e : x.getChildWithTagNameIterator ("Out"))
        {
            if (c >= 6) break;
            p.out[c++] = { e->getIntAttribute ("in"), (float) e->getDoubleAttribute ("level", 1.0), (float) e->getDoubleAttribute ("pan") };
        }
        int f = 0;
        for (auto* e : x.getChildWithTagNameIterator ("Filter"))
        {
            if (f >= 2) break;
            auto& fl = p.flt[f++];
            fl.bypass = e->getBoolAttribute ("bypass", true); fl.type = e->getIntAttribute ("type"); fl.cutoffHz = (float) e->getDoubleAttribute ("hz", 20000.0);
            fl.reso = (float) e->getDoubleAttribute ("reso"); fl.in = e->getIntAttribute ("in"); fl.eg = e->getIntAttribute ("eg"); fl.egInt = (float) e->getDoubleAttribute ("egInt");
        }
        bool anyOut = false;
        for (auto& o : p.out) anyOut = anyOut || (o.in > 0 && std::abs (o.level) > 0.0f);
        p.valid = anyOut;
        return p;
    }

    juce::String Patch::toString() const { return toXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()); }

    Patch Patch::fromString (const juce::String& s)
    {
        if (auto x = juce::parseXML (s)) return fromXml (*x);
        return {};
    }

    juce::String Patch::describe() const
    {
        juce::String s = dxAlgorithm > 0 ? "DX7 algorithm " + juce::String (dxAlgorithm) : juce::String ("MOD-7 patch");
        int n = 0;
        for (auto& o : op) n += o.on ? 1 : 0;
        return s + ", " + juce::String (n) + " oscillators";
    }

    //==========================================================================
    void Env::enter (int s, float target, float seconds)
    {
        stage = s;
        tgt = target;
        remaining = juce::jmax (1, (int) (seconds * sr));
        step = (tgt - lvl) / (float) remaining;
    }

    void Env::start (const EnvSettings& e, double sampleRate)
    {
        env = e;
        sr = sampleRate;
        lvl = e.start;
        enter (1, e.attackLevel, e.attackTime);
    }

    void Env::release()
    {
        if (stage == 0 || stage == 5) return;
        enter (5, env.releaseLevel, env.releaseTime);
    }

    float Env::advance (int n)
    {
        while (n > 0 && stage != 0 && stage != 4)
        {
            const int k = juce::jmin (n, remaining);
            lvl += step * (float) k;
            remaining -= k;
            n -= k;
            if (remaining <= 0)
            {
                lvl = tgt;
                if (stage == 1) enter (2, env.breakLevel, env.decayTime);
                else if (stage == 2) enter (3, env.sustain, env.slopeTime);
                else if (stage == 3) { stage = 4; lvl = env.sustain; }
                else if (stage == 5) stage = 0;
            }
        }
        return lvl;
    }

    //==========================================================================
    void Voice::start (const Patch& p, int noteNum, float vel01, double sampleRate)
    {
        sr = sampleRate;
        note = noteNum;
        for (int k = 0; k < 6; ++k) { phase[k] = 0.0; fb1[k] = fb2[k] = 0.0f; }
        for (auto& o : out) o = 0.0f;
        for (int e = 0; e < 9; ++e) eg[e].start (p.eg[e], sr);
        amp.start (p.ampEg, sr);
        for (int k = 0; k < 6; ++k)
        {
            const auto& o = p.op[k];
            // keyboard track: deo po deo linearno oko centralnog tastera (nagib u punoj jacini po oktavi)
            auto slope = [] (float ramp) { return std::tan (juce::jlimit (-0.98f, 0.98f, ramp) * juce::MathConstants<float>::halfPi) * 0.25f; };
            float f = 0.0f;
            if (noteNum >= o.ktCenter)
            {
                f += slope (o.ktRamp[2]) * (float) (juce::jmin (noteNum, o.ktHigh) - o.ktCenter) / 12.0f;
                if (noteNum > o.ktHigh) f += slope (o.ktRamp[3]) * (float) (noteNum - o.ktHigh) / 12.0f;
            }
            else
            {
                f += slope (o.ktRamp[1]) * (float) (o.ktCenter - juce::jmax (noteNum, o.ktLow)) / 12.0f;
                if (noteNum < o.ktLow) f += slope (o.ktRamp[0]) * (float) (o.ktLow - noteNum) / 12.0f;
            }
            opGain[k] = o.volume * juce::jlimit (0.0f, 4.0f, 1.0f + f) * juce::jmax (0.0f, velFactor (o.velSens, vel01));
        }
        velAmp = p.ampLevel * juce::jmax (0.0f, velFactor (p.ampVel, vel01)) * p.patchGain;
        svfIc1[0] = svfIc1[1] = svfIc2[0] = svfIc2[1] = 0.0f;
        dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0f;
        ctrl = 0;
    }

    void Voice::release()
    {
        for (auto& e : eg) e.release();
        amp.release();
    }

    void Voice::tick (const Patch& p, double noteHz, float& l, float& r)
    {
        if (--ctrl <= 0)
        {
            ctrl = 16;
            for (int e = 0; e < 9; ++e) egLevel[e + 1] = eg[e].advance (16);
            egLevel[10] = amp.advance (16);
            egLevel[0] = 1.0f;
        }
        float next[NumSrc] {};
        // sum
        rng = rng * 1664525u + 1013904223u;
        const float white = (float) (int) (rng >> 8) / 8388608.0f - 1.0f;
        next[Noise] = white * p.noiseVol * egLevel[juce::jlimit (0, 10, p.noiseEg)];

        for (int k = 0; k < 6; ++k)
        {
            const auto& o = p.op[k];
            if (! o.on) continue;
            float mod = 0.0f;
            for (auto& in : o.inputs) mod += out[juce::jlimit (0, NumSrc - 1, in.first)] * in.second;
            const float g = opGain[k] * egLevel[juce::jlimit (0, 10, o.eg)];
            float y = 0.0f;
            const double ph = phase[k];
            if (o.mode == 6)                 // waveshaper ulaza
                y = std::tanh (mod * (1.0f + juce::Decibels::decibelsToGain (o.drive)));
            else if (o.mode == 7)            // ring mod: ulazi pomnozeni
            {
                float a = 0.0f, b = 1.0f;
                int n = 0;
                for (auto& in : o.inputs) { const float v = out[juce::jlimit (0, NumSrc - 1, in.first)] * in.second; if (n++ == 0) a = v; else b = v; }
                y = n >= 2 ? a * b : a * (float) std::sin (juce::MathConstants<double>::twoPi * ph);
            }
            else
            {
                // fazna modulacija: pun izlaz modulatora = jedan ceo krug (kao DX7)
                const double fb = (double) o.feedback * 0.5 * (double) (fb1[k] + fb2[k]) * 0.5;
                const double x = ph + (double) mod + fb;
                const double fr = x - std::floor (x);
                switch (o.mode)
                {
                    case 2:  y = (float) (fr < 0.5 ? 4.0 * fr - 1.0 : 3.0 - 4.0 * fr); break;   // triangle
                    case 3:  y = fr < 0.5 ? 1.0f : -1.0f; break;                                 // square
                    case 4:  y = (float) (2.0 * fr - 1.0); break;                               // saw
                    default: y = (float) std::sin (juce::MathConstants<double>::twoPi * x); break;
                }
                if (o.mode == 5 && std::abs (o.drive) > 0.5f)
                    y = std::tanh (y * juce::Decibels::decibelsToGain (o.drive)) / std::tanh (juce::Decibels::decibelsToGain (o.drive));
            }
            y *= g;
            fb2[k] = fb1[k];
            fb1[k] = y;
            next[Osc1 + k] = y;
            const double hz = o.ratio * noteHz + o.offsetHz;
            phase[k] += hz / sr;
            if (phase[k] > 1.0e6) phase[k] -= std::floor (phase[k]);
        }
        for (int m = 0; m < 3; ++m)
            next[Mixer1 + m] = out[juce::jlimit (0, NumSrc - 1, p.mix[m].in[0])] * p.mix[m].level[0]
                             + out[juce::jlimit (0, NumSrc - 1, p.mix[m].in[1])] * p.mix[m].level[1];
        for (int f = 0; f < 2; ++f)
        {
            const auto& fl = p.flt[f];
            float x = out[juce::jlimit (0, NumSrc - 1, fl.in)];
            if (! fl.bypass && fl.in > 0)
            {
                // TPT state variable filter
                const float egOct = fl.eg > 0 ? fl.egInt * egLevel[juce::jlimit (0, 10, fl.eg)] * 6.0f : 0.0f;
                const float fc = juce::jlimit (20.0f, (float) (sr * 0.45), fl.cutoffHz * std::exp2 (egOct));
                const float gg = std::tan (juce::MathConstants<float>::pi * fc / (float) sr);
                const float k2 = 2.0f - 1.95f * fl.reso;
                const float a1 = 1.0f / (1.0f + gg * (gg + k2));
                const float v1 = a1 * (svfIc1[f] + gg * (x - svfIc2[f]));
                const float v2 = svfIc2[f] + gg * v1;
                svfIc1[f] = 2.0f * v1 - svfIc1[f];
                svfIc2[f] = 2.0f * v2 - svfIc2[f];
                const float hp = x - k2 * v1 - v2;
                x = fl.type == 1 ? hp : fl.type == 2 ? v1 : fl.type == 3 ? x - k2 * v1 : v2;
            }
            next[FilterA + f] = x;
        }
        // izlazni mikser
        float L = 0.0f, R = 0.0f;
        for (auto& o : p.out)
        {
            if (o.in <= 0) continue;
            const float v = next[juce::jlimit (0, NumSrc - 1, o.in)] * o.level;
            L += v * juce::jmin (1.0f, 1.0f - o.pan);
            R += v * juce::jmin (1.0f, 1.0f + o.pan);
        }
        std::copy (std::begin (next), std::end (next), std::begin (out));
        const float a = juce::jmax (0.0f, egLevel[10]) * velAmp * 0.5f;
        // DC blokada (~10 Hz): FM sa feedback-om / pomerenom fazom moze da da jednosmernu komponentu
        const float c = 1.0f - (float) (juce::MathConstants<double>::twoPi * 10.0 / sr);
        const float yl = L - dcX[0] + c * dcY[0]; dcX[0] = L; dcY[0] = yl;
        const float yr = R - dcX[1] + c * dcY[1]; dcX[1] = R; dcY[1] = yr;
        l = yl * a;
        r = yr * a;
    }
}

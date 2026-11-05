#include "MasterEffects.h"

void Biquad::set (Kind kind, double sampleRate, double freq, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / (2.0 * q);
    double B0, B1, B2, A0, A1, A2;

    switch (kind)
    {
        case LowShelf:
        {
            const double sq = 2.0 * std::sqrt (A) * alpha;
            B0 = A * ((A + 1) - (A - 1) * cw + sq);
            B1 = 2 * A * ((A - 1) - (A + 1) * cw);
            B2 = A * ((A + 1) - (A - 1) * cw - sq);
            A0 = (A + 1) + (A - 1) * cw + sq;
            A1 = -2 * ((A - 1) + (A + 1) * cw);
            A2 = (A + 1) + (A - 1) * cw - sq;
            break;
        }
        case HighPass:
        {
            B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = (1 + cw) / 2;
            A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha;
            break;
        }
        case HighShelf:
        {
            const double sq = 2.0 * std::sqrt (A) * alpha;
            B0 = A * ((A + 1) + (A - 1) * cw + sq);
            B1 = -2 * A * ((A - 1) + (A + 1) * cw);
            B2 = A * ((A + 1) + (A - 1) * cw - sq);
            A0 = (A + 1) - (A - 1) * cw + sq;
            A1 = 2 * ((A - 1) - (A + 1) * cw);
            A2 = (A + 1) - (A - 1) * cw - sq;
            break;
        }
        case Peak:
        default:
            B0 = 1 + alpha * A;
            B1 = -2 * cw;
            B2 = 1 - alpha * A;
            A0 = 1 + alpha / A;
            A1 = -2 * cw;
            A2 = 1 - alpha / A;
            break;
    }

    b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0;
    a1 = A1 / A0; a2 = A2 / A0;
}

//==============================================================================
//  PlateReverb (Jon Dattorro, "Effect Design Part 1", 1997)
//==============================================================================
void PlateReverb::prepare (double sampleRate)
{
    sr = sampleRate;
    lastSize = -1.0f;
    preDelay.init ((int) (0.25 * sampleRate));
    build (0.5f);
}

void PlateReverb::build (float size)
{
    // velicina menja duzine svih linija (0 = mala ploca, 1 = velika)
    scale = (float) (sr / 29761.0) * (0.55f + 0.9f * juce::jlimit (0.0f, 1.0f, size));
    const int inLens[4] = { 142, 107, 379, 277 };
    for (int i = 0; i < 4; ++i) { inAp[i].len = ms (inLens[i]); inAp[i].line.init (inAp[i].len); }
    const int ap1[2] = { 672, 908 }, d1[2] = { 4453, 4217 }, ap2[2] = { 1800, 2656 }, d2[2] = { 3720, 3163 };
    const int excursion = (int) std::ceil (16.0 * sr / 29761.0) + 2;
    for (int k = 0; k < 2; ++k)
    {
        tankAp1Len[k] = ms (ap1[k]);
        tankAp1[k].init (tankAp1Len[k] + 2 * excursion);
        tankDelay1Len[k] = ms (d1[k]);  tankDelay1[k].init (tankDelay1Len[k]);
        tankAp2[k].len = ms (ap2[k]);   tankAp2[k].line.init (tankAp2[k].len);
        tankDelay2Len[k] = ms (d2[k]);  tankDelay2[k].init (tankDelay2Len[k]);
        damp[k] = tankFeed[k] = 0.0f;
    }
    lastSize = size;
}

void PlateReverb::reset()
{
    preDelay.clear();
    for (auto& a : inAp) a.line.clear();
    for (int k = 0; k < 2; ++k)
    {
        tankAp1[k].clear(); tankDelay1[k].clear(); tankAp2[k].line.clear(); tankDelay2[k].clear();
        damp[k] = tankFeed[k] = 0.0f;
    }
    bwState = hpState = hpPrev = 0.0f;
}

void PlateReverb::process (float* l, float* r, int n, const MasterSettings& s)
{
    if (std::abs (s.reverbSize - lastSize) > 0.02f)
        build (s.reverbSize);

    const float fs = (float) sr;
    const int pre = juce::jlimit (0, (int) preDelay.buf.size() - 2, (int) (s.reverbPreDelayMs * 0.001f * fs));
    const float bw = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jlimit (500.0f, 18000.0f, s.reverbHighCut) / fs);
    const float hpCoef = std::exp (-juce::MathConstants<float>::twoPi * juce::jlimit (20.0f, 2000.0f, s.reverbLowCut) / fs);
    const float dampCoef = 0.05f + 0.8f * juce::jlimit (0.0f, 1.0f, s.reverbDamping);
    const float dif = juce::jlimit (0.0f, 1.0f, s.reverbDiffusion);
    const float inDif1 = 0.75f * dif, inDif2 = 0.625f * dif;
    const float decDif1 = 0.70f * (0.4f + 0.6f * dif), decDif2 = juce::jlimit (0.25f, 0.5f, 0.5f * (0.5f + 0.5f * dif));
    // jedna polovina rezervoara ~ (ap1 + d1 + ap2 + d2), dva puta se mnozi sa decay
    const float halfLoop = (float) (672 + 4453 + 1800 + 3720) * scale / fs;
    const float decay = juce::jlimit (0.0f, 0.985f, std::pow (10.0f, -3.0f * halfLoop / (2.0f * juce::jmax (0.2f, s.reverbDecay))));
    const float excursion = (float) (16.0 * sr / 29761.0) * juce::jlimit (0.0f, 1.0f, s.reverbMod);
    const double lfoInc = juce::MathConstants<double>::twoPi * 0.7 / sr;
    const float width = juce::jlimit (0.0f, 1.0f, s.reverbWidth);

    for (int i = 0; i < n; ++i)
    {
        // ulaz: mono, low cut (1-polni HP), predelay, high cut (bandwidth)
        float x = 0.5f * (l[i] + r[i]);
        const float hp = hpCoef * (hpState + x - hpPrev);
        hpPrev = x; hpState = hp;
        preDelay.write (hp);
        x = preDelay.read (pre);
        bwState += bw * (x - bwState);
        x = bwState;

        x = inAp[0].process (x, inDif1);
        x = inAp[1].process (x, inDif1);
        x = inAp[2].process (x, inDif2);
        x = inAp[3].process (x, inDif2);

        lfoPhase += lfoInc;
        if (lfoPhase > juce::MathConstants<double>::twoPi) lfoPhase -= juce::MathConstants<double>::twoPi;
        const float mod[2] = { (float) std::sin (lfoPhase) * excursion, (float) std::cos (lfoPhase) * excursion };

        float outFeed[2];
        for (int k = 0; k < 2; ++k)
        {
            float v = x + tankFeed[1 - k];
            // modulisani allpass (koeficijent sa obrnutim znakom, kao kod Dattorro)
            const float d = tankAp1[k].readFrac ((float) (tankAp1Len[k] - 1) + mod[k] + excursion);
            const float u = v + decDif1 * d;
            tankAp1[k].write (u);
            v = d - decDif1 * u;

            tankDelay1[k].write (v);
            v = tankDelay1[k].read (tankDelay1Len[k] - 1);
            damp[k] += (1.0f - dampCoef) * (v - damp[k]);
            v = damp[k] * decay;
            v = tankAp2[k].process (v, decDif2);
            tankDelay2[k].write (v);
            outFeed[k] = tankDelay2[k].read (tankDelay2Len[k] - 1) * decay;
        }
        tankFeed[0] = outFeed[0];
        tankFeed[1] = outFeed[1];

        // izlazni tapovi (Dattorro tabela), skalirani
        auto t = [this] (int v) { return ms (v); };
        float yl = tankDelay1[1].read (t (266)) + tankDelay1[1].read (t (2974)) - tankAp2[1].line.read (t (1913))
                 + tankDelay2[1].read (t (1996)) - tankDelay1[0].read (t (1990)) - tankAp2[0].line.read (t (187))
                 - tankDelay2[0].read (t (1066));
        float yr = tankDelay1[0].read (t (353)) + tankDelay1[0].read (t (3627)) - tankAp2[0].line.read (t (1228))
                 + tankDelay2[0].read (t (2673)) - tankDelay1[1].read (t (2111)) - tankAp2[1].line.read (t (335))
                 - tankDelay2[1].read (t (121));
        yl *= 0.6f; yr *= 0.6f;
        const float mid = 0.5f * (yl + yr), side = 0.5f * (yl - yr) * width;
        l[i] = mid + side;
        r[i] = mid - side;
    }
}

//==============================================================================
void MasterEffects::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 2 };

    reverb.prepare (sampleRate);
    delay.prepare (spec);
    delay.reset();
    delayTime.reset (sampleRate, 0.1);
    delayTime.setCurrentAndTargetValue ((float) (0.35 * sampleRate));

    low.reset(); mid.reset(); high.reset();
    lastLow = lastMid = lastHigh = 999.0f;

    compressor.prepare (spec);
    compressor.setAttack (10.0f);
    compressor.setRelease (150.0f);
    limiterGain = 1.0f;
    limiterReleaseCoef = 1.0f - std::exp (-1.0f / (0.08f * (float) sampleRate));

    masterGain.reset (sampleRate, 0.05);
}

void MasterEffects::process (juce::AudioBuffer<float>& main,
                             juce::AudioBuffer<float>& revSend,
                             juce::AudioBuffer<float>& dlySend,
                             int n, const MasterSettings& s, const GlobalEqSettings& g)
{
    auto* ml = main.getWritePointer (0);
    auto* mr = main.getWritePointer (1);

    // --- Reverb (send efekat) ---
    reverb.process (revSend.getWritePointer (0), revSend.getWritePointer (1), n, s);

    // --- Delay (send efekat) ---
    delayTime.setTargetValue ((float) (s.delayTimeMs * 0.001 * sr));
    auto* dl = dlySend.getWritePointer (0);
    auto* dr = dlySend.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float t = delayTime.getNextValue();
        const float yl = delay.popSample (0, t);
        const float yr = delay.popSample (1, t);
        delay.pushSample (0, dl[i] + yr * s.delayFeedback);
        delay.pushSample (1, dr[i] + yl * s.delayFeedback);
        dl[i] = yl;
        dr[i] = yr;
    }

    auto* rl = revSend.getReadPointer (0);
    auto* rr = revSend.getReadPointer (1);
    for (int i = 0; i < n; ++i)
    {
        ml[i] += rl[i] * s.reverbLevel + dl[i] * s.delayLevel;
        mr[i] += rr[i] * s.reverbLevel + dr[i] * s.delayLevel;
    }

    // --- EQ ---
    if (! juce::exactlyEqual (s.eqLowDb, lastLow))   { low.set  (Biquad::LowShelf,  sr, 120.0,  0.707, s.eqLowDb);  lastLow = s.eqLowDb; }
    if (! juce::exactlyEqual (s.eqMidDb, lastMid))   { mid.set  (Biquad::Peak,      sr, 1000.0, 0.8,   s.eqMidDb);  lastMid = s.eqMidDb; }
    if (! juce::exactlyEqual (s.eqHighDb, lastHigh)) { high.set (Biquad::HighShelf, sr, 7000.0, 0.707, s.eqHighDb); lastHigh = s.eqHighDb; }

    // EQ po performansu je uklonjen iz EFFECTS (EQ se podesava u partovima / userima, a za ceo zvuk je MASTER EQ):
    // stari performansi ga vise ne ukljucuju
    const bool eqOn = false && (std::abs (s.eqLowDb) > 0.05f || std::abs (s.eqMidDb) > 0.05f || std::abs (s.eqHighDb) > 0.05f);
    if (eqOn)
    {
        for (int i = 0; i < n; ++i)
        {
            ml[i] = high.process (0, mid.process (0, low.process (0, ml[i])));
            mr[i] = high.process (1, mid.process (1, low.process (1, mr[i])));
        }
    }

    // --- MASTER EQ (ceo Workstation) ---
    if (! gInit || g != gLast)
    {
        auto clampHz = [this] (double f, double lo, double hi) { return juce::jlimit (lo, juce::jmin (hi, sr * 0.45), f); };
        gHpf.set  (Biquad::HighPass,  sr, clampHz (g.hpfHz, 20.0, 400.0), 0.707, 0.0);
        gLow.set  (Biquad::LowShelf,  sr, clampHz (g.lowHz, 20.0, 1000.0), 0.707, g.lowDb);
        gM1.set   (Biquad::Peak,      sr, clampHz (g.m1Hz, 40.0, 16000.0), juce::jlimit (0.3, 8.0, (double) g.m1Q), g.m1Db);
        gM2.set   (Biquad::Peak,      sr, clampHz (g.m2Hz, 40.0, 16000.0), juce::jlimit (0.3, 8.0, (double) g.m2Q), g.m2Db);
        gHigh.set (Biquad::HighShelf, sr, clampHz (g.highHz, 1000.0, 18000.0), 0.707, g.highDb);
        for (int b = 0; b < GlobalEqSettings::numGraphic; ++b)
            gBand[(size_t) b].set (Biquad::Peak, sr, clampHz (GlobalEqSettings::graphicHz[b], 20.0, 20000.0), 2.15, g.g[(size_t) b]);   // 2/3 oktave
        if (! g.active() || g.mode != gLast.mode)
        {
            gHpf.reset(); gLow.reset(); gM1.reset(); gM2.reset(); gHigh.reset();
            for (auto& x : gBand) x.reset();
        }
        gLast = g; gInit = true;
    }
    if (g.active())
    {
        if (g.mode == 1)
        {
            for (int b = 0; b < GlobalEqSettings::numGraphic; ++b)
            {
                if (std::abs (g.g[(size_t) b]) <= 0.05f) continue;   // ravni opseg: bez racunanja
                auto& f = gBand[(size_t) b];
                for (int i = 0; i < n; ++i) { ml[i] = f.process (0, ml[i]); mr[i] = f.process (1, mr[i]); }
            }
        }
        else
        {
            const bool hp = g.hpfHz > 21.0f;
            for (int i = 0; i < n; ++i)
            {
                float l = ml[i], r = mr[i];
                if (hp) { l = gHpf.process (0, l); r = gHpf.process (1, r); }
                l = gHigh.process (0, gM2.process (0, gM1.process (0, gLow.process (0, l))));
                r = gHigh.process (1, gM2.process (1, gM1.process (1, gLow.process (1, r))));
                ml[i] = l; mr[i] = r;
            }
        }
    }

    auto block = juce::dsp::AudioBlock<float> (main).getSubBlock (0, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> ctx (block);

    // --- Kompresor: iskljucen (uklonjen iz EFFECTS), stari performansi ga vise ne ukljucuju ---
    if (false && s.compRatio > 1.01f)
    {
        compressor.setThreshold (s.compThresholdDb);
        compressor.setRatio (s.compRatio);
        compressor.process (ctx);
    }

    // --- Master jacina + limiter (stiti zvucnike) ---
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (s.masterDb, -60.0f));
    const float ceiling = 0.944f; // -0.5 dBFS
    for (int i = 0; i < n; ++i)
    {
        const float g = masterGain.getNextValue();
        const float l = ml[i] * g;
        const float r = mr[i] * g;
        const float peak = juce::jmax (std::abs (l), std::abs (r));
        const float target = peak > ceiling ? ceiling / peak : 1.0f;
        if (target < limiterGain) limiterGain = target;                              // odmah stisa
        else                      limiterGain += (target - limiterGain) * limiterReleaseCoef; // polako pusti
        ml[i] = l * limiterGain;
        mr[i] = r * limiterGain;
    }
}

//==============================================================================
juce::StringArray GlobalEqSettings::names()
{
    juce::StringArray s { "on", "mode", "hpfHz", "lowDb", "lowHz", "m1Db", "m1Hz", "m1Q", "m2Db", "m2Hz", "m2Q", "highDb", "highHz" };
    for (int i = 0; i < numGraphic; ++i) s.add ("g" + juce::String (i));
    return s;
}

bool GlobalEqSettings::setByName (const juce::String& k, float v)
{
    if (k == "on") on = v > 0.5f;
    else if (k == "mode") mode = v > 0.5f ? 1 : 0;
    else if (k == "hpfHz") hpfHz = juce::jlimit (20.0f, 400.0f, v);
    else if (k == "lowDb") lowDb = juce::jlimit (-15.0f, 15.0f, v);   else if (k == "lowHz") lowHz = juce::jlimit (20.0f, 1000.0f, v);
    else if (k == "m1Db") m1Db = juce::jlimit (-15.0f, 15.0f, v);     else if (k == "m1Hz") m1Hz = juce::jlimit (40.0f, 16000.0f, v);
    else if (k == "m1Q") m1Q = juce::jlimit (0.3f, 8.0f, v);
    else if (k == "m2Db") m2Db = juce::jlimit (-15.0f, 15.0f, v);     else if (k == "m2Hz") m2Hz = juce::jlimit (40.0f, 16000.0f, v);
    else if (k == "m2Q") m2Q = juce::jlimit (0.3f, 8.0f, v);
    else if (k == "highDb") highDb = juce::jlimit (-15.0f, 15.0f, v); else if (k == "highHz") highHz = juce::jlimit (1000.0f, 18000.0f, v);
    else if (k.startsWith ("g") && k.substring (1).containsOnly ("0123456789"))
    {
        const int i = k.substring (1).getIntValue();
        if (i < 0 || i >= numGraphic) return false;
        g[(size_t) i] = juce::jlimit (-12.0f, 12.0f, v);
    }
    else return false;
    return true;
}

float GlobalEqSettings::getByName (const juce::String& k) const
{
    if (k == "on") return on ? 1.0f : 0.0f;
    if (k == "mode") return (float) mode;
    if (k == "hpfHz") return hpfHz;
    if (k == "lowDb") return lowDb;   if (k == "lowHz") return lowHz;
    if (k == "m1Db") return m1Db;     if (k == "m1Hz") return m1Hz;   if (k == "m1Q") return m1Q;
    if (k == "m2Db") return m2Db;     if (k == "m2Hz") return m2Hz;   if (k == "m2Q") return m2Q;
    if (k == "highDb") return highDb; if (k == "highHz") return highHz;
    if (k.startsWith ("g")) { const int i = k.substring (1).getIntValue(); if (i >= 0 && i < numGraphic) return g[(size_t) i]; }
    return 0.0f;
}

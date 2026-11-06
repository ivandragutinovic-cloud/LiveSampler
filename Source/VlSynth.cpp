#include "VlSynth.h"

namespace Vl
{
// izmereni ostatak kasnjenja po modelu (semplova), test: VL pitch harness
static constexpr double kBrassScale = 1.040;   // usne vuku ton navise: cev i usne se postave za toliko nize
static double kTune[NumModels] = { 0.98, 2.05, 0.0, 0.0, 1.9 };

juce::StringArray modelNames()   { return { "Clarinet (reed)", "Saxophone (reed, conical)", "Brass (lips)", "Flute / frula (air jet)", "Bowed string" }; }
juce::StringArray sourceNames()  { return { "Velocity", "Velocity + aftertouch", "Breath controller (CC2)", "Expression (CC11)", "Mod wheel" }; }

#define VL_FIELDS X(model) X(source) X(pressure) X(embouchure) X(throat) X(position) X(noise) X(growl) \
                  X(vibDepth) X(vibRate) X(vibDelay) X(attack) X(release) X(brightness) X(levelDb) X(transpose) \
                  X(dynFilter) X(harmonic) X(body) X(size) X(breathNoise)

juce::String Patch::toString() const
{
    juce::XmlElement e ("VL");
   #define X(n) e.setAttribute (#n, (double) n);
    VL_FIELDS
   #undef X
    return e.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
}

Patch Patch::fromString (const juce::String& s)
{
    Patch p;
    if (auto x = juce::parseXML (s))
    {
       #define X(n) p.n = (decltype (p.n)) x->getDoubleAttribute (#n, (double) p.n);
        VL_FIELDS
       #undef X
    }
    p.model = juce::jlimit (0, NumModels - 1, p.model);
    p.source = juce::jlimit (0, NumSources - 1, p.source);
    return p;
}

Patch Patch::makeDefault (int model)
{
    Patch p;
    p.model = juce::jlimit (0, NumModels - 1, model);
    switch (p.model)
    {
        case Clarinet: p.pressure = 0.55f; p.embouchure = 0.5f; p.throat = 0.45f; p.noise = 0.08f; p.brightness = 0.5f;
                       p.dynFilter = 0.6f; p.harmonic = -0.2f; p.body = 0.5f; p.breathNoise = 0.25f; break;
        // sax, truba i flauta: izmereno prema Yamaha VL1 demo snimcima (alikvoti tiho / jako)
        case Sax:      p.pressure = 0.9f;  p.embouchure = 0.38f; p.throat = 0.9f; p.position = 0.22f; p.noise = 0.12f; p.brightness = 0.2f;
                       p.dynFilter = 0.88f; p.harmonic = 0.9f; p.body = 0.9f; p.breathNoise = 0.35f; break;
        case Brass:    p.pressure = 0.95f; p.embouchure = 0.55f; p.throat = 0.66f; p.noise = 0.02f; p.vibDepth = 0.28f; p.brightness = 0.75f;
                       p.dynFilter = 0.5f; p.harmonic = 0.9f; p.body = 0.9f; p.breathNoise = 0.12f; break;
        case Flute:    p.pressure = 0.8f;  p.embouchure = 0.55f; p.throat = 0.45f; p.position = 0.32f; p.noise = 0.25f; p.brightness = 0.6f; p.attack = 0.06f;
                       p.dynFilter = 0.6f; p.harmonic = 0.0f; p.body = 0.1f; p.breathNoise = 0.6f; p.vibDepth = 0.45f; break;
        case Bowed:    p.pressure = 0.6f;  p.embouchure = 0.5f; p.throat = 0.5f; p.position = 0.13f; p.noise = 0.0f; p.vibDepth = 0.4f; p.attack = 0.08f; p.release = 0.25f;
                       p.brightness = 0.6f; p.dynFilter = 0.5f; p.harmonic = 0.0f; p.body = 0.8f; p.breathNoise = 0.15f; break;
        default: break;
    }
    return p;
}

void State::prepare (int maxDelay)
{
    int n = 1;
    while (n < maxDelay + 8) n <<= 1;
    for (auto* l : { &a, &b })
    {
        l->buf.assign ((size_t) n, 0.0f);
        l->mask = n - 1;
        l->w = 0;
        l->d = 1.0;
        l->last = 0.0f;
    }
}

// fazno kasnjenje (u semplovima) one-pole lowpass-a y = (1-a)x + a*y1 na ucestanosti w
static double onePoleDelay (double a, double w)
{
    return std::atan2 (a * std::sin (w), 1.0 - a * std::cos (w)) / juce::jmax (1.0e-6, w);
}

static float loopDamp (const Patch& p) { return juce::jlimit (0.05f, 0.9f, 0.7f - 0.6f * p.throat); }

// duzine linija za trazenu visinu; filteri u petlji se oduzimaju (inace bi ton bio nizi), ostatak je izmeren
static double loopDelay (const Patch& p, double sr, double hz, double& d2)
{
    hz = juce::jlimit (16.0, 4000.0, hz);
    const double period = sr / hz, w = juce::MathConstants<double>::twoPi * hz / sr;
    const double pole = onePoleDelay (loopDamp (p), w);
    d2 = 0.0;
    switch (p.model)
    {
        case Clarinet: return juce::jmax (2.0, period * 0.5 - 0.5 - pole - kTune[Clarinet]);   // zatvorena cev: pola perioda
        case Sax:
        {
            const double L = juce::jmax (4.0, period - 0.5 - pole - kTune[Sax]);
            const double pos = juce::jlimit (0.05, 0.5, (double) p.position);
            d2 = juce::jmax (1.0, L * pos);
            return juce::jmax (1.0, L * (1.0 - pos));
        }
        case Brass: return juce::jmax (2.0, period * 2.0 * kBrassScale - kTune[Brass]);   // kao STK: duza cev, usne biraju mod
        case Flute:
        {
            const double loop = juce::jmax (4.0, (period * 2.0 - pole - 4.49 - kTune[Flute]) / (1.0 - 0.01362));   // izmereno
            const double ratio = juce::jlimit (0.08, 0.6, (double) p.position);
            d2 = juce::jmax (1.0, loop / (1.0 + ratio) * ratio);
            return juce::jmax (2.0, loop / (1.0 + ratio));
        }
        case Bowed:
        {
            const double base = juce::jmax (4.0, period - pole - kTune[Bowed]);
            const double beta = juce::jlimit (0.02, 0.45, (double) p.position);
            d2 = juce::jmax (1.0, base * beta);                          // most -> gudalo
            return juce::jmax (1.0, base * (1.0 - beta));                // gudalo -> vrat
        }
        default: return period;
    }
}

void Voice::setPitch (const Patch& p, double hz)
{
    if (state == nullptr) return;
    double d2 = 0.0;
    const double d1 = loopDelay (p, sr, hz, d2);
    const double maxD = (double) state->a.mask - 4.0;
    state->a.d = juce::jlimit (1.0, maxD, d1);
    state->b.d = juce::jlimit (1.0, maxD, d2 > 0.0 ? d2 : 1.0);
    lastHz = hz;
}

// RBJ biquad (peak / highpass / lowpass)
static void rbj (Voice::Bq& q, int type, double f, double Q, double gainDb, double sr)
{
    f = juce::jlimit (20.0, sr * 0.45, f);
    const double w = juce::MathConstants<double>::twoPi * f / sr, cw = std::cos (w), sw = std::sin (w), al = sw / (2.0 * Q);
    double b0, b1, b2, a0, a1, a2;
    if (type == 0)   // peak
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
    }
    else if (type == 1)   // highpass
    { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; }
    else                  // lowpass
    { b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; }
    q.b0 = (float) (b0 / a0); q.b1 = (float) (b1 / a0); q.b2 = (float) (b2 / a0); q.a1 = (float) (a1 / a0); q.a2 = (float) (a2 / a0);
}

// telo instrumenta: dve rezonance po modelu (Hz za srednju velicinu)
static void bodyFormants (int model, double& f1, double& f2)
{
    switch (model)
    {
        case Clarinet: f1 = 1500; f2 = 3100; break;
        case Sax:      f1 = 560;  f2 = 1550; break;   // alt; size pomera ka sopranu / tenoru / baritonu
        case Brass:    f1 = 1250; f2 = 2900; break;
        case Flute:    f1 = 900;  f2 = 2300; break;
        case Bowed:    f1 = 290;  f2 = 3000; break;   // vazdusna rezonanca tela + "most"
        default:       f1 = 800;  f2 = 2500; break;
    }
}

void Voice::updateModifiers (const Patch& p, double hz)
{
    // dinamicki filter: dah otvara (tiho tamno, jako svetlo), blago prati visinu
    const float b = juce::jlimit (0.0f, 1.2f, env);
    const double fc = juce::jlimit (150.0, sr * 0.45, 220.0 * std::exp2 (p.brightness * 5.0 + p.dynFilter * 5.0 * (b - 0.5))
                                                     * std::pow (juce::jmax (30.0, hz) / 261.6, 0.4));
    svfG = (float) std::tan (juce::MathConstants<double>::pi * fc / sr);
    svfK = 1.2f;   // malo rezonance na ivici (Q ~0.83)
    const double sz = std::exp2 ((0.5 - p.size) * 1.4);
    double f1, f2;
    bodyFormants (p.model, f1, f2);
    rbj (body1, 0, f1 * sz, 1.6, p.body * 9.0, sr);
    rbj (body2, 0, f2 * sz, 2.2, p.body * 6.0, sr);
    rbj (nzHp, 1, 900.0, 0.7, 0.0, sr);
    radCoef = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 900.0 * sz / sr));
    rbj (nzLp, 2, 6000.0 * sz, 0.7, 0.0, sr);
}

void Voice::start (const Patch& p, State* s, int note, float vel01, double sampleRate)
{
    sr = sampleRate;
    state = s;
    if (state != nullptr) { state->a.clear(); state->b.clear(); state->inUse = true; }
    active = state != nullptr;
    releasing = false;
    vel = vel01;
    env = 0.0f;
    attackCoef  = 1.0f - std::exp (-1.0f / (juce::jmax (0.002f, p.attack)  * 0.3f * (float) sr));
    releaseCoef = 1.0f - std::exp (-1.0f / (juce::jmax (0.005f, p.release) * 0.3f * (float) sr));
    filt = filt2 = dcX = dcY = tone = lipY1 = lipY2 = outX = outY = 0.0f;
    svfIc1 = svfIc2 = hDc = hDcY = softLp = radLp = 0.0f;
    for (auto* b : { &body1, &body2, &nzHp, &nzLp }) { b->z1 = b->z2 = 0.0f; }
    modCount = 0;
    time = 0.0; vibPhase = 0.0; growlPhase = 0.0;
    rng = 22222u + (juce::uint32) note * 7919u;
    quietSamples = 0;
    setPitch (p, 440.0 * std::exp2 ((note - 69) / 12.0));
}

float Voice::tick (const Patch& p, double hz, float breathCtl, float aftertouch) noexcept
{
    if (! active || state == nullptr) return 0.0f;

    // dah: koliko jako se duva (0..1)
    float amount = 0.0f;
    const float velShape = std::pow (vel, 0.6f);
    switch (p.source)
    {
        case Velocity:            amount = velShape; break;
        case VelocityAftertouch:  amount = juce::jlimit (0.0f, 1.2f, velShape * (0.75f + 0.6f * aftertouch)); break;
        default:                  amount = juce::jlimit (0.0f, 1.0f, breathCtl); break;
    }
    const float target = releasing ? 0.0f : amount;
    env += (target - env) * (releasing ? releaseCoef : attackCoef);

    // vibrato (posle kasnjenja) i growl
    time += 1.0 / sr;
    const double vibAmt = time > p.vibDelay ? juce::jmin (1.0, (time - p.vibDelay) / 0.4) : 0.0;
    vibPhase += juce::MathConstants<double>::twoPi * p.vibRate / sr;
    if (vibPhase > juce::MathConstants<double>::twoPi) vibPhase -= juce::MathConstants<double>::twoPi;
    const float vib = (float) (std::sin (vibPhase) * vibAmt) * p.vibDepth;
    // vibrato visine (VL1 demo: ~4.5-5.5 Hz, oko 15-25 centi), plus malo daha u ritmu vibrata
    hz *= std::exp2 ((double) vib * 50.0 / 1200.0);
    if (std::abs (hz - lastHz) > lastHz * 0.0002 + 0.001) setPitch (p, hz);
    growlPhase += juce::MathConstants<double>::twoPi * 31.0 / sr;
    if (growlPhase > juce::MathConstants<double>::twoPi) growlPhase -= juce::MathConstants<double>::twoPi;
    const float growl = p.growl * (0.6f * (float) std::sin (growlPhase) + 0.4f * noiseSample());

    const float n = noiseSample();
    // na pocetku tona jaci "udarac" vazduha: oscilacija krene odmah (inace sax treba ~0.5 s da progovori)
    const float kick = time < 0.08 ? 0.03f * (float) (1.0 - time / 0.08) : 0.0f;
    const float excite = (0.002f + kick) * n * juce::jmin (1.0f, env * 20.0f);   // malo suma uvek: pokrece oscilaciju (bez toga petlja moze da ostane mirna)
    float out = 0.0f;
    const float damp = loopDamp (p);   // prigusenje u petlji (one-pole)

    switch (p.model)
    {
        case Clarinet:
        case Sax:
        {
            // ispod ~0.55 jezicak ne oscilluje: tihi dah krece od praga, jaci ide preko
            float breath = (p.model == Sax ? 0.59f : 0.5f) * juce::jmin (1.0f, env * 8.0f) + env * (p.model == Sax ? 0.05f + 0.1f * p.pressure : 0.2f + 0.3f * p.pressure);
            breath += breath * (p.noise * 0.25f * n + growl * 0.5f + vib * 0.08f) + excite;
            const float slope = -(0.18f + 0.3f * p.embouchure);
            const float offset = 0.7f;
            auto reed = [&] (float x) { return juce::jlimit (-1.0f, 1.0f, offset + slope * x); };
            const float oz = 0.5f * (state->a.last + filt2); filt2 = state->a.last;   // one-zero (kao STK)
            if (p.model == Clarinet)
            {
                filt = (1.0f - damp) * oz + damp * filt;
                float diff = -0.95f * filt - breath;
                out = state->a.tick (breath + diff * reed (diff));
            }
            else
            {
                filt = (1.0f - damp) * oz + damp * filt;
                const float temp = -0.95f * filt;
                const float last = temp - state->b.last;
                const float diff = breath - last;
                state->b.tick (temp);
                state->a.tick (breath - diff * juce::jlimit (-1.0f, 1.0f, 0.7f + (0.18f + 0.3f * p.embouchure) * diff) - temp);
                out = last * 0.45f;
            }
            break;
        }
        case Brass:
        {
            float breath = 0.6f * juce::jmin (1.0f, env * 8.0f) + env * (0.3f + 0.4f * p.pressure);
            breath += breath * (p.noise * 0.1f * n + growl * 0.5f + 0.01f * n) + vib * 0.02f + excite;
            const float mouth = 0.3f * breath;
            const float bore = (0.8f + 0.12f * p.throat) * state->a.last;
            // usne: rezonantni filter malo iznad tona (embouchure = napetost usana)
            const double lipHz = hz / kBrassScale * std::exp2 ((p.embouchure - 0.5) * 0.15);
            const double wl = juce::MathConstants<double>::twoPi * lipHz / sr;
            const float r = (float) juce::jlimit (0.99, 0.9985, 1.0 - 0.09 * wl);      // sirina rezonance usana prati visinu
            const float a1 = -2.0f * r * (float) std::cos (wl), a2 = r * r;
            const float x = mouth - bore;
            const float lg = (float) (0.003 * (1.0 - r) / 0.003 * std::sin (wl) / 0.0342); // isto pojacanje na svim visinama
            const float y = lg * x - a1 * lipY1 - a2 * lipY2;
            lipY2 = lipY1; lipY1 = y;
            float area = juce::jmin (1.0f, y * y);
            float v = area * mouth + (1.0f - area) * bore;
            // DC blok
            const float dc = v - dcX + 0.995f * dcY; dcX = v; dcY = dc;
            out = state->a.tick (dc) * 2.0f;
            break;
        }
        case Flute:
        {
            float breath = 0.95f * juce::jmin (1.0f, env * 8.0f) + env * (0.15f + 0.3f * p.pressure);
            float rnd = (p.noise * 0.6f * n + vib * 0.05f + growl * 0.3f) * breath + excite;
            filt = (1.0f - damp) * state->a.last + damp * filt;
            float t = -filt;
            const float dc = t - dcX + 0.995f * dcY; dcX = t; dcY = dc; t = dc;
            float pd = breath + rnd - 0.5f * t;
            pd = state->b.tick (pd);
            pd = juce::jlimit (-1.0f, 1.0f, pd * (pd * pd - 1.0f)) + 0.5f * t;
            out = 0.6f * state->a.tick (pd);
            break;
        }
        case Bowed:
        {
            const float bowV = env * (0.03f + 0.22f * p.pressure) * (1.0f + growl * 0.3f + p.noise * 0.05f * n);
            filt = (1.0f - damp) * state->b.last + damp * filt;
            const float bridgeRefl = -0.95f * filt;
            const float nutRefl = -state->a.last;
            const float strV = bridgeRefl + nutRefl;
            const float dv = bowV - strV;
            const float slope = 5.0f - 4.0f * p.embouchure;
            float bt = std::abs ((dv + 0.001f) * slope) + 0.75f;
            bt = juce::jlimit (0.01f, 0.98f, std::pow (bt, -4.0f));
            const float nv = releasing && env < 0.02f ? 0.0f : dv * bt;
            state->a.tick (bridgeRefl + nv);
            state->b.tick (nutRefl + nv);
            out = state->b.last * 2.5f;
            break;
        }
        default: break;
    }

    static constexpr float modelGain[NumModels] = { 0.75f, 1.25f, 1.2f, 0.58f, 0.75f };   // izjednacene jacine modela
    out *= modelGain[p.model];

    // DC blok na izlazu (jezicak i mlaz daju i jednosmernu komponentu)
    const float hp = out - outX + 0.997f * outY; outX = out; outY = hp; out = hp;

    // --- modifikatori (VL1: Harmonic Enhancer, Dynamic Filter, Resonator) ---
    if (--modCount <= 0) { modCount = 32; updateModifiers (p, hz); }
    // Harmonic Enhancer: + dodaje parne / neparne alikvote (oštrije, "trubastije"), - zaobljava
    if (p.harmonic > 0.0f)
    {
        const float h = p.harmonic;
        float x = out * (1.0f + 1.5f * h);
        x = x + h * (0.45f * x * x + 0.3f * x * x * x);
        const float d = x - hDc + 0.995f * hDcY; hDc = x; hDcY = d;   // parni clan pravi DC
        out = d / (1.0f + 1.2f * h);
    }
    else if (p.harmonic < 0.0f)
    {
        softLp += 0.25f * (out - softLp);
        out = out * (1.0f + p.harmonic) - p.harmonic * softLp;
    }
    // sum daha koji se cuje (filtriran, prati dah)
    out += p.breathNoise * 0.08f * juce::jmin (1.0f, env) * nzLp.process (nzHp.process (n));
    // dinamicki filter (TPT state variable, lowpass)
    {
        const float g = svfG, k = svfK;
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const float v3 = out - svfIc2;
        const float v1 = a1 * svfIc1 + a2 * v3;
        const float v2 = svfIc2 + a2 * svfIc1 + a3 * v3;
        svfIc1 = 2.0f * v1 - svfIc1;
        svfIc2 = 2.0f * v2 - svfIc2;
        out = v2;
    }
    // zvono zraci visoke tonove jace od niskih (truba, sax): prigusi osnovu ispod ~1 kHz
    {
        static constexpr float radiation[NumModels] = { 0.3f, 0.45f, 0.85f, 0.0f, 0.0f };
        const float k = radiation[p.model] * (0.4f + 0.6f * p.body);
        radLp += radCoef * (out - radLp);
        out -= k * radLp;
    }
    // telo / zvono
    out = body2.process (body1.process (out));
    float y = std::tanh (out * 1.1f) * juce::Decibels::decibelsToGain (p.levelDb) * (0.3f + 0.7f * juce::jmin (1.0f, env));
    if (! std::isfinite (y)) { y = 0.0f; state->a.clear(); state->b.clear(); }

    // kraj glasa: pusten i utihnuo
    if (releasing && env < 0.0005f && std::abs (y) < 0.0005f) { if (++quietSamples > 2048) { active = false; state->inUse = false; } }
    else quietSamples = 0;
    return y;
}
}

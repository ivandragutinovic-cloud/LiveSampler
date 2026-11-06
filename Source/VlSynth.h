#pragma once
#include <JuceHeader.h>

// VL: fizicko modelovanje duvackih i gudackih instrumenata (talasovod, kao Yamaha VL1 / Stanford STK).
// Zvuk ne dolazi iz semplova: pobuda (jezicak, usne, vazdusni mlaz, gudalo) pokrece vazdusni stub / zicu
// u petlji sa kasnjenjem. Svira se "dahom": jacina, aftertouch, breath controller (CC2) ili expression (CC11).
namespace Vl
{
    constexpr const char* instrumentName = "VL physical model";

    enum Model { Clarinet = 0, Sax, Brass, Flute, Bowed, NumModels };
    enum BreathSource { Velocity = 0, VelocityAftertouch, BreathCC2, ExpressionCC11, ModWheel, NumSources };

    juce::StringArray modelNames();
    juce::StringArray sourceNames();

    struct Patch
    {
        int model = Sax;
        int source = VelocityAftertouch;
        float pressure = 0.6f;      // koliko jako se duva / vuce gudalo (0..1)
        float embouchure = 0.5f;    // jezicak / usne: mekano (0) .. tvrdo (1); kod gudala pritisak gudala
        float throat = 0.5f;        // grlo: prigusenje petlje (0 tamno .. 1 otvoreno, svetlo)
        float position = 0.5f;      // sax: mesto jezicka, flauta: odnos mlaza, gudalo: mesto gudala
        float noise = 0.15f;        // sum vazduha
        float growl = 0.0f;         // rezanje (growl)
        float vibDepth = 0.15f;     // vibrato (polutonovi * 0.5)
        float vibRate = 5.5f;       // Hz
        float vibDelay = 0.35f;     // s posle pocetka tona
        float attack = 0.04f;       // s
        float release = 0.12f;      // s
        float brightness = 0.6f;    // tonski filter na izlazu
        float levelDb = 0.0f;
        int transpose = 0;
        // modifikatori (kao kod VL1): boja prati dah, alikvoti, telo instrumenta, sum daha koji se cuje
        float dynFilter = 0.6f;     // koliko dah otvara filter (tiho = tamno, jako = svetlo)
        float harmonic = 0.0f;      // -1 mekano / baršunasto .. +1 oštro / trubasto
        float body = 0.6f;          // rezonanca tela / zvona
        float size = 0.5f;          // velicina instrumenta (sopran .. bariton): pomera rezonance tela
        float breathNoise = 0.3f;   // sum vazduha koji se cuje oko tona

        juce::String toString() const;
        static Patch fromString (const juce::String&);
        static Patch makeDefault (int model);
    };

    // stanje jednog glasa (linije kasnjenja); drzi ga SamplerPart u malom bazenu, ne alocira se na audio niti
    struct State
    {
        void prepare (int maxDelay);
        struct Line
        {
            std::vector<float> buf;
            int w = 0, mask = 0;
            double d = 1.0;
            float last = 0.0f;
            float tick (float in) noexcept
            {
                buf[(size_t) w] = in;
                double rp = (double) w - d;
                while (rp < 0.0) rp += (double) (mask + 1);
                const int i0 = (int) rp;
                const float f = (float) (rp - i0);
                const float a = buf[(size_t) (i0 & mask)], b = buf[(size_t) ((i0 + 1) & mask)];
                // citanje unazad: izmedju i0 i i0+1 (i0+1 je noviji)
                last = a + (b - a) * f;
                w = (w + 1) & mask;
                return last;
            }
            void clear() { std::fill (buf.begin(), buf.end(), 0.0f); last = 0.0f; }
        };
        Line a, b;
        bool inUse = false;
    };

    class Voice
    {
    public:
        void start (const Patch& p, State* s, int note, float vel01, double sampleRate);
        void release() { releasing = true; }
        // hz = trenutna visina (bend, glide, vibrato vec uracunati spolja ako treba); breathCtl = kontroler 0..1
        float tick (const Patch& p, double hz, float breathCtl, float aftertouch) noexcept;
        bool isActive() const { return active; }
        State* state = nullptr;
        struct Bq { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
                    float process (float x) noexcept { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };


    private:
        double sr = 44100.0;
        bool active = false, releasing = false;
        float vel = 1.0f, env = 0.0f, attackCoef = 0.0f, releaseCoef = 0.0f;
        float filt = 0.0f, filt2 = 0.0f, dcX = 0.0f, dcY = 0.0f, tone = 0.0f;
        float lipY1 = 0.0f, lipY2 = 0.0f, outX = 0.0f, outY = 0.0f;
        // modifikatori
        Bq body1, body2, nzHp, nzLp;
        float svfIc1 = 0.0f, svfIc2 = 0.0f, svfG = 0.5f, svfK = 1.4f;
        float hDc = 0.0f, hDcY = 0.0f, softLp = 0.0f;
        int modCount = 0;
        void updateModifiers (const Patch& p, double hz);
        double time = 0.0, vibPhase = 0.0, growlPhase = 0.0;
        juce::uint32 rng = 22222u;
        int quietSamples = 0;
        double lastHz = 0.0;
        float noiseSample() noexcept
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return (float) (rng & 0xffffff) / (float) 0x7fffff - 1.0f;
        }
        void setPitch (const Patch& p, double hz);
    };
}

#pragma once
#include <JuceHeader.h>
#include "Model.h"

// FM sinteza (Korg MOD-7 / DX7 / V50): svira MOD-7 programe iz Kronos PCG-a uzivo, bez semplova.
// Patch se cita iz EXi bloka MOD-7 programa (Prog_EXi.txt). Podrzano: 6 VPM oscilatora (sine, triangle,
// square, saw), FM ulazi i feedback, DX7 algoritmi (template 1-32) ili patch kablovi, EG1-9 + Amp EG,
// velocity, keyboard track, 3 miksera, 2 filtera, sum, izlazni mikser. PCM oscilator i waveshaper tabele
// (nisu dokumentovane) se ne sviraju - zvuk je blizak originalu, ne identican.
namespace Fm
{
    constexpr const char* instrumentName = "FM synth (MOD-7)";

    struct Patch
    {
        struct Op
        {
            bool on = false;
            int mode = 1;                       // 0 off, 1 sine, 2 triangle, 3 square, 4 saw, 5 sine+waveshaper, 6 waveshaper (ulaz), 7 ring mod
            double ratio = 1.0, offsetHz = 0.0;
            std::vector<std::pair<int, float>> inputs;   // izvor (vidi Src) i nivo
            float feedback = 0.0f;              // 0..1
            float volume = 1.0f;                // 0..1 (linearno)
            int eg = 0;                         // 0 nijedan, 1-9 EG1-9, 10 Amp EG
            float velSens = 0.0f;               // -1..1
            int ktLow = 0, ktCenter = 60, ktHigh = 127;
            float ktRamp[4] {};                 // low, center-low, center-high, high (-1..1)
            float drive = 0.0f;                 // waveshaper (priblizno: meko zasicenje)
        };
        Op op[6];
        EnvSettings eg[9], ampEg;
        float ampLevel = 1.0f, ampVel = 0.0f;
        struct Mix { int in[2] { 0, 0 }; float level[2] { 1.0f, 1.0f }; } mix[3];
        struct Out { int in = 0; float level = 1.0f, pan = 0.0f; } out[6];
        struct Filter { bool bypass = true; int type = 0; float cutoffHz = 20000.0f, reso = 0.0f; int in = 0; int eg = 0; float egInt = 0.0f; } flt[2];
        float noiseVol = 0.0f; int noiseEg = 0;
        int transpose = 0;
        float patchGain = 1.0f;
        int dxAlgorithm = 0;                    // 1-32 = DX7 template, 0 = patch kablovi
        bool valid = false;

        // block = 1052 bajta EXi bloka (od 2908 za EXi1, 3960 za EXi2) MOD-7 programa
        static Patch fromMod7 (const juce::uint8* block, const juce::uint8* program, int exiSlot);
        static Patch makeInit();                       // novi FM user: jednostavan DX patch (2 operatora)
        void applyDxAlgorithm (int n);                 // veze i nosioci po DX7 algoritmu 1-32
        std::unique_ptr<juce::XmlElement> toXml() const;
        static Patch fromXml (const juce::XmlElement&);
        juce::String toString() const;                 // za cuvanje u useru
        static Patch fromString (const juce::String&);
        juce::String describe() const;
    };

    // izvori signala (Input Jack): 0 nista, 1 PCM, 2 sum, 3 audio ulaz, 4-9 OSC1-6, 10-12 Mixer1-3, 13-14 Filter A/B
    // DX7 operator n (1-6) = OSC (7-n) = indeks 6-n
    inline int opIndex (int dxOp) { return 6 - dxOp; }
    std::vector<int> dxCarriers (int algorithm);
    std::vector<std::pair<int, int>> dxModulations (int algorithm);   // (operator, njegov modulator)

    enum Src { None = 0, Pcm = 1, Noise = 2, AudioIn = 3, Osc1 = 4, Mixer1 = 10, FilterA = 13, FilterB = 14, NumSrc = 15 };

    class Env
    {
    public:
        void start (const EnvSettings& e, double sampleRate);
        void release();
        float advance (int n);
        float level() const { return lvl; }
        bool active() const { return stage != 0; }
    private:
        void enter (int s, float target, float seconds);
        EnvSettings env;
        double sr = 44100.0;
        int stage = 0;           // 0 idle, 1 attack, 2 decay, 3 slope, 4 sustain, 5 release
        float lvl = 0.0f, tgt = 0.0f, step = 0.0f;
        int remaining = 0;
    };

    class Voice
    {
    public:
        void start (const Patch& p, int note, float vel01, double sampleRate);
        void release();
        bool isActive() const { return amp.active(); }
        // jedan sempl; noteHz = visina tona (sa bend-om, glide-om...)
        void tick (const Patch& p, double noteHz, float& l, float& r);
    private:
        double sr = 44100.0;
        double phase[6] {};
        float out[NumSrc] {};
        float fb1[6] {}, fb2[6] {};
        float opGain[6] {};
        Env eg[9], amp;
        float egLevel[11] {};
        int ctrl = 0;
        float velAmp = 1.0f;
        juce::uint32 rng = 22222;
        float svfIc1[2] {}, svfIc2[2] {};
        float dcX[2] {}, dcY[2] {};
        int note = 60;
    };
}

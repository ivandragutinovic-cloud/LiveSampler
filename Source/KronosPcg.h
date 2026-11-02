#pragma once
#include <JuceHeader.h>

// Citac Korg Kronos PCG fajla (programi, kombiji) i KSC kolekcija semplova.
// Format nije javno opisan; ovo je ono sto je utvrdjeno na pravim fajlovima.
namespace Kronos
{
    struct Zone
    {
        bool used = false;     // ima multisempl (user ili ROM)
        bool user = false;     // user multisempl iz KSC kolekcije (ne fabricki ROM)
        juce::String uuid;     // UUID KSC kolekcije (hex, mala slova)
        int ms = 0;            // broj multisempla u kolekciji
        int value = 127;       // bajt posle broja (verovatno jacina zone) - proverava se u fazi 2
    };

    // Parametri jednog oscilatora (OSC1/OSC2) - sirove vrednosti kao na Kronosu
    struct OscParams
    {
        int keyLow = 0, keyHigh = 127;
        int velLow = 1, velHigh = 127;
        std::array<int, 8> zoneBottom { 1, 1, 1, 1, 1, 1, 1, 1 };  // donja jacina zone (zona 1 je najjaca)
        std::array<int, 8> xfadeRange {};   // 0 = Off
        std::array<int, 8> xfadeCurve {};   // 0 Linear, 1 Power, 2 Layer
        int transpose = 0, tuneCents = 0;
        int lfo1Freq = 0, pitchLfo1Int = 0, pitchEgInt = 0;
        int portaOn = 0, portaTime = 0, bendUp = 2, bendDown = -2;
        int filterRouting = 0, cutoff = 99, resonance = 0, filterEgInt = 0;
        std::array<int, 9> filterEg {};   // start, attack t, attack lvl, decay t, break, slope t, sustain, release t, release lvl
        std::array<int, 8> ampEg {};      // start, attack t, attack lvl, decay t, break, slope t, sustain, release t
        int ampLevel = 127, pan = 64;
    };

    struct Program
    {
        juce::uint32 bankId = 0;
        juce::String bank;     // npr. "USER-G"
        int number = 0;
        juce::String name;
        bool hd1 = false;      // HD-1 (semplovi); EXi programi se ne uvoze
        std::array<std::array<Zone, 8>, 2> osc;
        std::array<OscParams, 2> params;
        bool doubleMode = false, mono = false, legato = false;
        int priority = 2;      // 0 najniza, 1 najvisa, 2 poslednja

        bool usesUser() const;
        int firstUserZone (int oscIndex) const;
        juce::String id() const { return bank + " " + juce::String (number).paddedLeft ('0', 3); }
    };

    struct Combi
    {
        juce::uint32 bankId = 0;
        juce::String bank;
        int number = 0;
        juce::String name;
    };

    struct Pcg
    {
        juce::File file;
        std::vector<Program> programs;
        std::vector<Combi> combis;
        int drumKits = 0;
        bool load (const juce::File& f, juce::String& error);
    };

    juce::String programBankName (juce::uint32 id);
    juce::String combiBankName (juce::uint32 id);

    // offseti u HD-1 programu (4960 bajtova)
    constexpr int programSize = 4960;
    constexpr int osc1ZonesOffset = 2774;
    constexpr int osc2ZonesOffset = 3240;
    constexpr int zoneSize = 22;
}

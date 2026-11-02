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

    struct Program
    {
        juce::uint32 bankId = 0;
        juce::String bank;     // npr. "USER-G"
        int number = 0;
        juce::String name;
        bool hd1 = false;      // HD-1 (semplovi); EXi programi se ne uvoze
        std::array<std::array<Zone, 8>, 2> osc;

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

#pragma once
#include <JuceHeader.h>

// Registri: zapamcena kombinacija instrumenata i svih podesavanja delova.
// 4 banke x 8 registara = 32, MIDI Program Change 0-31 ih poziva.
class Registers
{
public:
    static constexpr int numBanks = 4;
    static constexpr int perBank = 8;
    static constexpr int numSlots = numBanks * perBank;
    static constexpr int numParts = 3;

    struct Slot
    {
        bool used = false;
        juce::NamedValueSet values;          // paramID -> normalizovana vrednost
        juce::String instruments[numParts];
    };

    Slot& operator[] (int i) { return slots[(size_t) juce::jlimit (0, numSlots - 1, i)]; }
    const Slot& operator[] (int i) const { return slots[(size_t) juce::jlimit (0, numSlots - 1, i)]; }

    void loadFrom (const juce::File& file);
    void saveTo (const juce::File& file) const;

    // svi instrumenti koji se pominju u registrima (za unapred ucitavanje)
    juce::StringArray getAllInstrumentNames() const;

private:
    std::array<Slot, numSlots> slots;
};

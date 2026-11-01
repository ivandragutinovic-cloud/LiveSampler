#pragma once
#include <JuceHeader.h>
#include "MasterEffects.h"

// Podaci koje korisnik pravi i cuva: User (zvuk od 8 oscilatora), Performans (8 delova), Terce.

constexpr int kNumOsc = 8;
constexpr int kNumParts = 8;
constexpr int kNumPerfBanks = 4;
constexpr int kPerfPerBank = 16;
constexpr int kNumPerfSlots = kNumPerfBanks * kPerfPerBank;

//==============================================================================
struct OscSettings
{
    bool on = false;
    juce::String instrument;            // ime iz biblioteke
    float levelDb = 0.0f, pan = 0.0f;
    int transpose = 0;                  // polutonovi
    float fineCents = 0.0f;
    int keyLow = 0, keyHigh = 127;
    int velLow = 1, velHigh = 127;

};

struct UserProgram
{
    juce::String name { "Novi user" };
    std::array<OscSettings, kNumOsc> osc;

    float attack = 0.001f, decay = 0.5f, sustain = 1.0f, release = 0.3f;
    float cutoff = 20000.0f, resonance = 0.0f;
    float velSens = 0.8f;
    bool mono = false;
    float portamento = 0.0f;
    int bendRange = 2;
    int fxType = 0;
    float fxRate = 0.3f, fxDepth = 0.5f, fxMix = 0.5f;

    std::unique_ptr<juce::XmlElement> toXml() const;
    static UserProgram fromXml (const juce::XmlElement&);
    juce::StringArray usedInstruments() const;
};

//==============================================================================
struct PerfPart
{
    bool on = false;
    juce::String user;                  // ime usera
    float volumeDb = 0.0f, pan = 0.0f;
    int transpose = 0, octave = 0;
    int keyLow = 0, keyHigh = 127;
    int velLow = 1, velHigh = 127;
    int midiChannel = 0;                // 0 = svi kanali
    float reverbSend = 0.2f, delaySend = 0.0f;
    bool terca = false;                 // ovaj deo dobija terce
    bool sustainPedal = true;
};

struct Performance
{
    juce::String name { "Init" };
    std::array<PerfPart, kNumParts> parts;
    MasterSettings master;
    bool used = false;

    std::unique_ptr<juce::XmlElement> toXml() const;
    static Performance fromXml (const juce::XmlElement&);
    static Performance makeDefault (const juce::String& firstUser);
};

//==============================================================================
//  Terce
//==============================================================================
struct TercaScale
{
    juce::String name;
    std::array<int, 12> above {};       // za ton (polutonova od tonike): koliko polutonova gore
    std::array<int, 12> below {};       // ... i dole (negativno). 0 = bez terce na tom tonu
};

struct TercaRule
{
    int mask = 0;                       // 12 bitova, tonovi akorda od tonike (bit 0 = tonika)
    int scale = 0;                      // indeks skale
    int rootShift = 0;                  // tonalitet = tonika akorda + pomeraj (npr. E7 -> A mol: +5)
};

struct LearnTrigger
{
    enum Type { None = 0, CC, Note };
    int type = None;
    int channel = 0;                    // 1-16, 0 = bilo koji
    int number = 0;

    bool matchesCC (int ch, int cc) const    { return type == CC && number == cc && (channel == 0 || channel == ch); }
    bool matchesNote (int ch, int note) const { return type == Note && number == note && (channel == 0 || channel == ch); }
    juce::String describe() const;
};

struct TercaSettings
{
    std::vector<TercaScale> scales;
    std::vector<TercaRule> rules;
    int voiceMode = 0;                  // 0 = iznad, 1 = ispod, 2 = oba
    float velocity = 0.8f;              // jacina terce u odnosu na melodiju
    bool topNoteOnly = true;            // terca samo na najvisi ton (melodija)
    LearnTrigger learn;

    static TercaSettings makeDefault();
    std::unique_ptr<juce::XmlElement> toXml() const;
    static TercaSettings fromXml (const juce::XmlElement&);
};

namespace Terca
{
    // prepoznaje tonalitet iz skupa tonova (12-bitna maska apsolutnih tonova C=bit0)
    // vraca true i postavlja root (0-11) i skalu
    bool recognize (const TercaSettings& s, int pitchClassMask, int& root, int& scale);

    // terca za notu: vraca do 2 note (u out), broj nota
    int harmonize (const TercaSettings& s, int root, int scale, int note, int out[2]);

    juce::String maskToText (int mask);           // "0 4 7" -> "C E G" relativno (1, 3, 5)
    juce::String keyName (int root, const TercaSettings& s, int scale);
    juce::String pcName (int pc);
}

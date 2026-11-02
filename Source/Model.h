#pragma once
#include <JuceHeader.h>
#include "MasterEffects.h"

// Podaci koje korisnik pravi i cuva: User (zvuk od 8 oscilatora), Performans (8 delova), Terce.

constexpr int kMsPerOsc = 8;                 // MS1..MS8 u OSC1 i u OSC2 (kao Kronos)
constexpr int kNumOsc = 2 * kMsPerOsc;       // 0..7 = OSC1, 8..15 = OSC2
constexpr int kNumParts = 16;               // kao 16 timbre-a u Kronos kombiju
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
    int block = 0;                      // 0 = OSC1, 1 = OSC2 (uvek index / kMsPerOsc)
    int fadeIn = 0, fadeOut = 0;        // velocity crossfade (koliko koraka od donje / gornje granice)
    int fadeCurve = 0;                  // 0 linearno, 1 power
};

// Envelope kao na Kronosu: start -> attack nivo -> break nivo -> sustain -> release nivo
struct EnvSettings
{
    float start = 0.0f;
    float attackTime = 0.001f, attackLevel = 1.0f;
    float decayTime = 0.3f, breakLevel = 1.0f;
    float slopeTime = 0.3f, sustain = 1.0f;
    float releaseTime = 0.3f, releaseLevel = 0.0f;
};

namespace FilterType { enum { Off = 0, LP24, LP12, HP, BP, Num }; }

struct SoundBlock
{
    // pitch
    float pitchEgInt = 0.0f;            // polutonovi na pun nivo pitch EG-a
    EnvSettings pitchEg { 0.0f, 0.001f, 0.0f, 0.1f, 0.0f, 0.1f, 0.0f, 0.1f, 0.0f };
    float vibRate = 5.5f, vibDepth = 0.0f, vibDelay = 0.3f;   // Hz, centi, sekunde
    float mwToVib = 30.0f, atToVib = 0.0f;                    // centi

    // filter
    int filterType = FilterType::LP24;
    float cutoff = 20000.0f, resonance = 0.0f;
    float keyTrack = 0.0f;              // 1 = prati klavijaturu potpuno
    float velToCutoff = 0.0f;           // oktave (pozitivno: jaci udarac = svetlije)
    float filterEgInt = 0.0f;           // oktave
    float velToFilterEg = 0.0f;         // oktave EG-a na pun udarac (Kronos: Filter EG Velocity to A)
    float bendToCutoff = 0.0f;          // oktave na pun pitch bend (gore otvara, dole zatvara)
    EnvSettings filterEg { 0.0f, 0.01f, 1.0f, 0.5f, 0.4f, 0.5f, 0.2f, 0.5f, 0.0f };
    float filterLfoRate = 2.0f, filterLfoDepth = 0.0f;        // Hz, oktave
    float mwToCutoff = 0.0f, atToCutoff = 0.0f;               // oktave

    // amp
    EnvSettings ampEg { 0.0f, 0.001f, 1.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.3f, 0.0f };
    float velSens = 0.8f, velCurve = 1.6f;
    float atToAmp = 0.0f;               // dB
};

namespace VoiceMode { enum { Poly = 0, Mono, MonoLegato }; }
namespace Priority  { enum { Last = 0, High, Low }; }

struct UserProgram
{
    juce::String name { "Novi user" };
    std::array<OscSettings, kNumOsc> osc;
    std::array<SoundBlock, 2> block;

    int voiceMode = VoiceMode::Poly;
    int priority = Priority::Last;
    bool portaOn = false;
    float portamento = 0.1f;            // sekunde (kod konstantne brzine: po oktavi)
    bool portaFingered = false;         // klizi samo kad se svira vezano
    bool portaConstRate = false;
    int bendUp = 2, bendDown = 2;

    // EQ zvuka (prenosi se u svaki performans; tamo moze dodatno da se dotera)
    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqMidFreq = 1000.0f, eqHighDb = 0.0f;

    int fxType = 0;
    float fxRate = 0.3f, fxDepth = 0.5f, fxMix = 0.5f;

    bool isMono() const { return voiceMode != VoiceMode::Poly; }

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
    float detuneCents = 0.0f;
    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqHighDb = 0.0f;   // dodaje se na EQ usera
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

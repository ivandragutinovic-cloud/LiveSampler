#pragma once
#include <JuceHeader.h>
#include "MasterEffects.h"

// Podaci koje korisnik pravi i cuva: User (zvuk od 8 oscilatora), Performans (8 delova), Terce.

constexpr int kMsPerOsc = 8;                 // MS1..MS8 u svakom OSC-u (kao Kronos)
constexpr int kNumBlocks = 8;                // OSC1..OSC8 (Kronos koristi OSC1 i OSC2)
constexpr int kNumOsc = kNumBlocks * kMsPerOsc;   // index = OSC * 8 + MS
constexpr int kNumParts = 16;               // kao 16 timbre-a u Kronos kombiju
constexpr int kNumPerfBanks = 16;            // 16 lista ...
constexpr int kPerfPerBank = 128;            // ... po 128 performansa (kao Kronos)
constexpr int kNumSetLists = 16;
constexpr int kSetListSlots = 128;
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
    int fadeOutFrom = 0;                // od koje jacine pocinje utisavanje (Kronos: donja granica gornje zone); 0 = velHigh - fadeOut + 1
    int fadeCurve = 0;                  // 0 linearno, 1 power
    float velRampDb = 0.0f;             // izjednacena dinamika slojeva: jacina na donjoj granici MS-a (dB, gornja = 0)
    bool keyOff = false;                // svira kad se pusti taster (release / keyoff sempl)
    // Key follow (Kronos Pitch Slope): 1 = normalno, 0 = ista visina na svim tasterima, 0.5 = pola tona po tasteru...
    float keySlope = 1.0f;
    // jedan sempl na svim tasterima: zona sa ovog tastera svira na celom rasponu MS-a (-1 = iskljuceno, svaki taster svoju zonu)
    int zoneKey = -1;
    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqMidFreq = 1000.0f, eqHighDb = 0.0f;   // EQ ovog MS-a

    // jacina ovog MS-a za datu dinamiku (velocity crossfade + izjednacena dinamika), bez Level-a
    float velocityGain (int velocity) const
    {
        if (velocity < velLow || velocity > velHigh) return 0.0f;
        auto fade = [this] (int steps, int dist)
        {
            if (steps <= 0) return 1.0f;
            const float x = juce::jlimit (0.0f, 1.0f, (float) (dist + 1) / (float) (steps + 1));
            return fadeCurve == 1 ? std::sin (x * juce::MathConstants<float>::halfPi) : x;
        };
        float g = fade (fadeIn, velocity - velLow);
        // utisavanje ide od fadeOutFrom navise (kao na Kronosu): moze da se zavrsi i iznad 127
        const int outStart = fadeOut > 0 ? (fadeOutFrom > 0 ? fadeOutFrom : velHigh - fadeOut + 1) : velHigh + 1;
        g *= fade (fadeOut, outStart + fadeOut - 1 - velocity);
        if (velRampDb < 0.0f)
        {
            const int top = juce::jmax (velLow + 1, juce::jmin (velHigh, outStart - 1));
            const float t = juce::jlimit (0.0f, 1.0f, (float) (velocity - velLow) / (float) (top - velLow));
            g *= juce::Decibels::decibelsToGain (velRampDb * (1.0f - t));
        }
        return g;
    }

    bool hasEq() const { return std::abs (eqLowDb) > 0.05f || std::abs (eqMidDb) > 0.05f || std::abs (eqHighDb) > 0.05f; }
};

// Envelope kao na Kronosu: start -> attack nivo -> break nivo -> sustain -> release nivo
struct EnvSettings
{
    float start = 0.0f;
    float attackTime = 0.001f, attackLevel = 1.0f;
    float decayTime = 0.3f, breakLevel = 1.0f;
    float slopeTime = 0.3f, sustain = 1.0f;
    float releaseTime = 0.3f, releaseLevel = 0.0f;
    // jacina udarca menja EG (Kronos: Level Mod / Time Mod sa izvorom Velocity), na pun udarac:
    float velStart = 0.0f, velAttack = 0.0f, velBreak = 0.0f;   // dodaje se na nivo (-1..1)
    float velAttackT = 0.0f, velDecayT = 0.0f, velSlopeT = 0.0f, velReleaseT = 0.0f;   // -1 = vreme /8, +1 = x8
    bool hasVelMod() const { return velStart != 0.0f || velAttack != 0.0f || velBreak != 0.0f || velAttackT != 0.0f
                                    || velDecayT != 0.0f || velSlopeT != 0.0f || velReleaseT != 0.0f; }
    // EG za datu jacinu (0..1)
    EnvSettings forVelocity (float v01) const
    {
        if (! hasVelMod()) return *this;
        EnvSettings e = *this;
        auto lvl = [v01] (float base, float m) { return juce::jlimit (-1.0f, 1.0f, base + m * v01); };
        auto tm = [v01] (float t, float m) { return t * std::pow (2.0f, 3.0f * m * v01); };
        e.start = lvl (start, velStart);
        e.attackLevel = lvl (attackLevel, velAttack);
        e.breakLevel = lvl (breakLevel, velBreak);
        e.attackTime = tm (attackTime, velAttackT);
        e.decayTime = tm (decayTime, velDecayT);
        e.slopeTime = tm (slopeTime, velSlopeT);
        e.releaseTime = tm (releaseTime, velReleaseT);
        return e;
    }
};

namespace FilterType { enum { Off = 0, LP24, LP12, HP, BP, Num }; }

struct SoundBlock
{
    bool on = true;                     // OSC on/off (all its MS)
    float drive = 0.0f, lowBoost = 0.0f;   // Kronos Driver: zasicenje (0..1) i pojacanje basa (0..1)
    float noteDelay = 0.0f;                // Note-On Delay: OSC krece ovoliko posle udarca (s)
    // pitch
    float pitchEgInt = 0.0f;            // polutonovi na pun nivo pitch EG-a
    EnvSettings pitchEg { 0.0f, 0.001f, 0.0f, 0.1f, 0.0f, 0.1f, 0.0f, 0.1f, 0.0f };
    float vibRate = 5.5f, vibDepth = 0.0f, vibDelay = 0.3f;   // Hz, centi, sekunde
    float vibFade = 0.25f, vibOffset = 0.0f;                  // sekunde, -1..1 (pomera centar LFO-a)
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

    // mono: samo tasteri u ovom opsegu sviraju mono/legato/portamento, ostali poly bez portamenta
    int monoLow = 0, monoHigh = 127;

    // prelaz (kao zica preko pragova): kad se drzi ton i odsvira sledeci, interval bira OSC.
    // Ukljuceni OSC-ovi cine grupu; OSC bez intervala je osnovni (prvi ton), ostali se pale na svoje intervale.
    bool articOn = false;
    int articMask = 0;                  // bit k = interval od k+1 polustepena (1..24)
    int articDir = 0;                   // 0 gore i dole, 1 samo gore, 2 samo dole

    // amp
    EnvSettings ampEg { 0.0f, 0.001f, 1.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.3f, 0.0f };
    float velSens = 0.8f, velCurve = 1.6f;
    float atToAmp = 0.0f;               // dB
};

namespace VoiceMode { enum { Poly = 0, Mono, MonoLegato, MonoLegatoXfade }; }   // Xfade: legato prelazi u sempl novog tona
namespace Priority  { enum { Last = 0, High, Low }; }

struct UserProgram
{
    juce::String name { "New user" };
    std::array<OscSettings, kNumOsc> osc;
    std::array<SoundBlock, kNumBlocks> block;

    int voiceMode = VoiceMode::Poly;
    // Mono legato crossfade: novi ton svira svoj sempl (svoju zonu), pretapa se iz prethodnog bez novog attack-a
    float legatoXfade = 0.06f;          // vreme pretapanja (s)
    float legatoStart = 0.05f;          // novi sempl krece ovoliko posle pocetka (preskace attack), s
    int priority = Priority::Last;
    bool portaOn = false;
    float portamento = 0.1f;            // sekunde (kod konstantne brzine: po oktavi)
    bool portaFingered = false;         // klizi samo kad se svira vezano
    bool portaConstRate = false;
    int bendUp = 2, bendDown = 2;

    // EQ zvuka (prenosi se u svaki performans; tamo moze dodatno da se dotera)
    float eqLowDb = 0.0f, eqMidDb = 0.0f, eqMidFreq = 1000.0f, eqHighDb = 0.0f;

    float levelDb = 0.0f;               // jacina celog usera (COMMON > Level), vazi u svim performansima
    bool levelLock = false;             // AUTO LEVEL ne dira ovog usera
    float send1 = 1.0f, send2 = 1.0f;   // koliko ovaj zvuk salje u reverb / delay (mnozi send dela)
    juce::String category;                  // kategorija usera (lista USER)
    juce::String kronosPcg, kronosSource;   // napravljen od Kronos programa: ime PCG fajla i "bankId:broj"
    juce::String originalXml;               // kako je user izgledao kad je napravljen (USER > ORIGINAL vraca na to)
    // FM sinteza: ceo MOD-7 program (hex, 4960 bajtova) i koji EXi slot (0/1) je MOD-7; prazno = semplovi
    juce::String fmProgram;
    int fmSlot = 0;
    juce::String fmPatch;               // FM parametri (Fm::Patch kao XML); ima prednost nad fmProgram (posle editovanja)
    bool isFm() const { return fmPatch.isNotEmpty() || fmProgram.isNotEmpty(); }
    juce::String vlPatch;               // VL fizicko modelovanje (Vl::Patch); prazno = nije VL
    bool isVl() const { return vlPatch.isNotEmpty(); }
    juce::String pluginDesc, pluginState;   // VST3 plugin: opis (XML PluginDescription) i stanje (base64); prazno = nije plugin
    bool isPlugin() const { return pluginDesc.isNotEmpty(); }

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
    float tercaVel = -1.0f;             // jacina terce samo za ovaj part (0.05..1.5), -1 = generalna (HARMONY Velocity)
    // ugradjena terca iz Kronos kombija (timbre transponovan +3/+4/-8... kroz user skalu, bez "Use Program Scale"):
    // part uvek svira tercu; harmIv = interval (polutonova, sa oktavom) za svaki ton od tonaliteta 0..11
    bool fixedHarm = false;
    std::array<int, 12> harmIv {};
    bool sustainPedal = true;
    bool expression = true;             // prima Expression (CC11)
    bool scaleTune = true;              // prati orijentalno stimovanje performansa (iskljuci za bubnjeve)
    int vectorDir = 0;                  // vector joystick: 0 Off, 1 +X, 2 -X, 3 +Y, 4 -Y (kao VJS Assign na Kronosu)
    float vectorCenter = 1.0f;          // jacina u centru joysticka (0..1, Volume Center)
    // Kronos program koji jos nije presemplovan: part ceka (cuti) dok se ne presempluje ili ne zameni drugim userom
    juce::String missingName;           // npr. "USER-A 112 Real Drums Kit"
    juce::String missingSource;         // "bankId:broj" programa u PCG-u
    juce::String missingPcg;            // ceo put do PCG fajla
    bool isMissing() const { return user.isEmpty() && missingSource.isNotEmpty(); }
    void clearMissing() { missingName = {}; missingSource = {}; missingPcg = {}; }
};

struct SetListSlot
{
    int perf = -1;                      // performans (lista * 128 + broj), -1 = prazno
    juce::String label;                 // tekst na dugmetu (prazno = ime performansa)
    int colour = 0;
};

struct SetList
{
    juce::String name;
    std::array<SetListSlot, kSetListSlots> slots;
};

// Stimovanja (makami): obrazac od tonike u centima; Tuning::apply ga postavlja na izabranu toniku
namespace Tuning
{
    struct Preset { const char* name; float cents[12]; };
    const std::vector<Preset>& presets();
    std::array<float, 12> apply (const Preset& p, int root);
}

// Scena (kao Montage Scene / Genos registracija): stanje delova u istom performansu, prelaz je trenutan (semplovi su vec ucitani)
constexpr int kNumScenes = 8;
struct ScenePart
{
    bool on = false;
    float volumeDb = 0.0f, pan = 0.0f;
    int transpose = 0, octave = 0;
    bool terca = false;
    int keyLow = 0, keyHigh = 127;
    float reverb = 0.2f, delay = 0.0f;
};
struct Scene
{
    bool used = false;
    juce::String name;
    std::array<ScenePart, kNumParts> parts;
};

struct Performance
{
    juce::String name { "Init" };
    std::array<PerfPart, kNumParts> parts;
    MasterSettings master;
    bool used = false;
    juce::String category;              // ime kategorije (prazno ili nepostojece = bez kategorije)
    bool vectorOn = false;              // vector joystick menja jacinu delova (Enable Volume Control)
    bool vectorEqualPower = false;      // pretapanje bez pada jacine u sredini
    juce::String kronosSource;          // kombi sa Kronosa od kog je napravljen: "bankId:broj"
    juce::String kronosPcg;             // ... iz kog PCG fajla (ime fajla)
    std::array<Scene, kNumScenes> scenes;
    int scene = -1;                     // poslednja pozvana scena (-1 = nijedna)
    // orijentalno stimovanje (Scale Tune / cetvrttonovi): pomeraj u centima za svaki ton C..B
    bool tuneOn = false;
    std::array<float, 12> tuneCents {};
    juce::String tuneName;
    // tonalitet za partove sa ugradjenom tercom (fixedHarm): -1 = nema. harmNative = skala (Major/Minor) za koju je harmIv napravljen
    int harmRoot = -1, harmScale = 0, harmNative = 0;
    bool hasFixedHarm() const { for (auto& p : parts) if (p.fixedHarm) return true; return false; }

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

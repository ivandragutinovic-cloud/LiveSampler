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
        int value = 127;       // jacina zone
        juce::String romKey;   // fabricki ROM / EXs multisempl: "rom:" + 20 bajtova zone (tip, banka, broj)
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
        int lfo1Freq = 0, lfo1Offset = 0, lfo1Delay = 0, lfo1Fade = 0;
        int pitchLfo1Int = 0, pitchLfo1JsY = 0, pitchEgInt = 0;
        int octave = 0;                   // -2..+1 (32' .. 4')
        int portaOn = 0, portaFingered = 0, portaConstTime = 0, portaTime = 0, bendUp = 2, bendDown = -2;
        int filterRouting = 0, cutoff = 99, resonance = 0, filterEgInt = 0, filterEgVel = 0;
        std::array<int, 9> filterEg {};   // start, attack t, attack lvl, decay t, break, slope t, sustain, release t, release lvl
        std::array<int, 8> ampEg {};      // start, attack t, attack lvl, decay t, break, slope t, sustain, release t
        int ampLevel = 127, ampVelocity = 0, pan = 64;
        int send1 = 127, send2 = 127;     // Send MFX1/MFX2 (vazi kad je Bus Select = L/R)
    };

    struct Program
    {
        juce::uint32 bankId = 0;
        juce::String bank;     // npr. "USER-G"
        int number = 0;
        juce::String name;
        bool hd1 = false;      // HD-1 (semplovi)
        bool exi = false;      // EXi program (AL-1, CX-3, STR-1, MS-20EX, PolysixEX, MOD-7, SGX-2, EP-1)
        std::array<int, 2> exiAlg {};   // Algorithm Type EXi1 / EXi2 (0 = Off, vidi exiEngineName)
        juce::String exiKey;   // "exi:" + otisak celog programa: presemplovan EXi program (Kronos Autosampler)
        juce::String drumKey;  // "drum:" + otisak: presemplovan drum kit program (svaki taster posebno)
        int exiSend1 = 127, exiSend2 = 127;
        std::vector<juce::uint8> raw;   // ceo program (samo EXi programi sa MOD-7: FM sinteza ih svira uzivo)
        std::array<std::array<Zone, 8>, 2> osc;
        std::array<OscParams, 2> params;
        bool doubleMode = false, mono = false, legato = false;
        bool drumKit = false;
        bool busLR = false;    // Bus Select L/R (inace ide kroz insert efekat)  // Oscillator Mode: Drums / Double Drums (zona pokazuje na drum kit)
        int priority = 2;      // 0 najniza, 1 najvisa, 2 poslednja
        // Track EQ programa (Prog_HD-1 / Prog_EXi_Common 2580-2585)
        bool eqBypass = false;
        float eqLowDb = 0.0f, eqMidDb = 0.0f, eqHighDb = 0.0f, eqMidFreq = 1000.0f;

        bool usesUser() const;
        int firstUserZone (int oscIndex) const;
        juce::String id() const { return bank + " " + juce::String (number).paddedLeft ('0', 3); }
        juce::String engineText() const;   // "HD-1", "AL-1", "AL-1+STR-1" ...
    };

    // Timbre u kombiju (16 komada, svaki 188 bajtova od mesta 4802)
    struct Timbre
    {
        juce::uint32 programBank = 0xffffffff;   // isti ID kao kod banke programa; 0xffffffff = nepoznata banka
        int programNumber = 0;
        bool play = false;                       // Play / Mute
        int channel = 0;                         // MIDI kanal 1-16, 0 = Gch (globalni)
        int volume = 127, pan = 64;
        int transpose = 0, detuneCents = 0;
        int keyLow = 0, keyHigh = 127, velLow = 1, velHigh = 127;
        int send1 = 0, send2 = 0;
        bool damper = true, footPedal = true;    // MIDI Filter: Damper (sustain), Foot Pedal (expression)
        int vectorDir = 0;                       // VJS Assign: 0 Off, 1 +X, 2 -X, 3 +Y, 4 -Y
        int vectorCenter = 4;                    // Volume Center: 0..4 = 0/25/50/75/100%
        // (Track EQ) timbre-a (CombiAndSongTimbreSet: timbre + 46..51)
        bool eqBypass = false;
        float eqLowDb = 0.0f, eqMidDb = 0.0f, eqHighDb = 0.0f, eqMidFreq = 1000.0f;
    };

    struct Combi
    {
        juce::uint32 bankId = 0;
        juce::String bank;
        int number = 0;
        juce::String name;
        std::array<Timbre, 16> timbres;
        int return1 = 127, return2 = 127;   // MFX1 / MFX2 Return
        int category = 0;                   // Combi Category 0-17 (imena u Pcg::combiCategories)
        bool vectorVolume = false, vectorEqualPower = false;   // Vector Control: Enable Volume Control, Equal Power
        juce::String id() const { return bank + " " + juce::String (number).paddedLeft ('0', 3); }
    };

    struct Pcg
    {
        juce::File file;
        std::vector<Program> programs;
        std::vector<Combi> combis;
        int drumKits = 0;
        int globalChannel = 1;   // Kronos GLOBAL MIDI kanal (iz GLB1)
        juce::StringArray combiCategories;   // imena kategorija kombija sa Kronosa (GLOBAL, 18 komada)
        bool load (const juce::File& f, juce::String& error);
        const Program* findProgram (juce::uint32 bankId, int number) const;
    };

    juce::String programBankName (juce::uint32 id);
    juce::String exiEngineName (int algorithmType);
    juce::String makeExiKey (const juce::uint8* program);   // "exi:" + otisak (4960 bajtova)

    // EXi program (Prog_EXi_Common.txt): Algorithm Type EXi1 / EXi2
    constexpr int exi1AlgOffset = 2857;
    constexpr int exi2AlgOffset = 3909;
    juce::String combiBankName (juce::uint32 id);

    // offseti u HD-1 programu (4960 bajtova)
    constexpr int programSize = 4960;
    constexpr int osc1ZonesOffset = 2774;
    constexpr int osc2ZonesOffset = 3240;
    constexpr int zoneSize = 22;
    constexpr int combiSize = 7810;
    constexpr int timbresOffset = 4802;
    constexpr int timbreSize = 188;
}

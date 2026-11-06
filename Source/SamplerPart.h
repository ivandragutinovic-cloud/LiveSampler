#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"
#include "Model.h"
#include "MasterEffects.h"
#include "FmSynth.h"
#include "VlSynth.h"

// Sve sto deo performansa treba za sviranje (nepromenljivo dok traje jedan audio blok)
struct HostedPlugin;
struct PartRuntime
{
    PerfPart part;
    UserProgram user;
    std::array<InstrumentPtr, kNumOsc> inst;
    std::shared_ptr<const Fm::Patch> fm;   // FM sinteza (user napravljen od MOD-7 programa)
    std::shared_ptr<const Vl::Patch> vl;   // VL fizicko modelovanje (duvaci, gudalo)
    std::shared_ptr<HostedPlugin> plugin;  // VST3 plugin user (zvuk pravi plugin, part radi jacinu, EQ, efekte)
    std::array<float, 12> scaleCents {};
    float harmGain = 1.0f;                 // jacina terce (HARMONY Velocity), mnozi glasove terce
    int maxVoices = 128;                   // polifonija ovog dela (SETTINGS); preko toga se krade najstariji glas   // orijentalno stimovanje za ovaj deo (nule = iskljuceno)
    bool active = false;   // ukljucen i ima usera
    bool vectorOn = false, vectorEqualPower = false;   // iz performansa
};

//==============================================================================
// Insert efekat jednog dela
class InsertFx
{
public:
    enum Type { Off = 0, Chorus, Phaser, Tremolo, Overdrive, Echo, NumTypes };
    static juce::StringArray getTypeNames()
    {
        return { "No effect", "Chorus", "Phaser", "Tremolo", "Overdrive", "Echo (delay)" };
    }

    void prepare (double sampleRate, int maxBlock);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, int type, float rate, float depth, float mix);

private:
    double sr = 44100.0;
    int lastType = -1;
    juce::AudioBuffer<float> dry;
    juce::dsp::Chorus<float> chorus;
    juce::dsp::Phaser<float> phaser;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 192000 };
    juce::SmoothedValue<float> delayTimeSmoothed;
    double tremPhase = 0.0;
    float odLowpassState[2] {};
};

//==============================================================================
// Envelope kao na Kronosu (linearni segmenti izmedju nivoa)
class KronosEnv
{
public:
    enum Stage { Idle = 0, Attack, Decay, Slope, Sustain, Release };

    void start (const EnvSettings& e, double sampleRate);
    // legato: bez attack-a, od trenutnog nivoa prethodnog tona ka sustain-u
    void startLegato (const EnvSettings& e, double sampleRate, float fromLevel);
    void release();
    void kill() { stage = Idle; level = 0.0f; }
    // napreduje n semplova, vraca nivo
    float advance (int n);
    float getLevel() const noexcept { return level; }
    bool isActive() const noexcept { return stage != Idle; }
    Stage getStage() const noexcept { return stage; }

private:
    void enter (Stage s, float target, float seconds);

    EnvSettings env;
    double sr = 44100.0;
    Stage stage = Idle;
    float level = 0.0f, target = 0.0f, step = 0.0f;
    bool expRelease = false;     // release ka nuli ide ravnomerno po dB (kao Kronos), ne po jacini
    double relMul = 1.0;
    int remaining = 0;
};

//==============================================================================
// Jedan deo performansa: user sa do 8 oscilatora (Zvuk A/B), insert efekat
class SamplerPart
{
public:
    void prepare (double sampleRate, int maxBlock);

    // audio nit
    void beginBlock (const PartRuntime* runtime, int numSamples);
    // id: 0-127 taster, 128-255 terca (prvi glas), additive = drugi glas terce (interno 256-383)
    void noteOn (int id, int pitch, int velocity, bool additive = false);
    void noteOff (int id);
    void setSustain (bool down);
    void setPitchBend (float minus1to1) { bend = minus1to1; }
    void setModWheel (float v) { modWheel = v; }
    void setAftertouch (float v) { aftertouch = v; }
    void setVector (float x, float y) { vecX = x; vecY = y; }
    void setExpression (float v) { expression = v; rawExpression = v; }
    void setBreath (float v) { breath = v; }   // breath controller (CC2) za VL usere
    // VL: bazen stanja (linije kasnjenja) za fizicki model; pravi se na message niti kad part dobije VL usera
    void ensureVlPool();
    void addExternal (const juce::AudioBuffer<float>& b, int numSamples);   // izlaz plugina ide kroz part (jacina, pan, EQ, send)
    void allNotesOff (bool immediate);
    void renderVoices (int startSample, int numSamples);
    void endBlock (juce::AudioBuffer<float>& mainOut, juce::AudioBuffer<float>& reverbSend,
                   juce::AudioBuffer<float>& delaySend, int numSamples);

    int getNumActiveVoices() const;
    // VOICE GUARD (audio nit): kratko (6 ms) utisa do maxCount najstarijih glasova ciji je taster pusten;
    // includePedal = i one koje drzi samo sustain pedala. Tasteri koji su pritisnuti se nikad ne diraju.
    int shedVoices (int maxCount, bool includePedal);
    std::atomic<int> stolenVoices { 0 };   // glasovi uzeti zbog limita polifonije (UI cita i brise)
    // VU: najveci vrh izlaza ovog dela od poslednjeg citanja (UI cita i brise)
    float takePeak (int ch) { return peak[ch & 1].exchange (0.0f); }
    bool offline = false;   // merenje van audio niti (AUTO LEVEL): bez citanja sa diska, samo pocetak sempla iz RAM-a
    static constexpr int numIds = 384;
    static constexpr int controlRate = 16;   // modulacija se racuna na svakih 16 semplova

private:
    std::atomic<float> peak[2] { 0.0f, 0.0f };
    struct Voice
    {
        bool active = false;
        int id = -1;
        juce::uint32 serial = 0;
        bool keyDown = false, held = false;
        InstrumentPtr instrument;
        const SampleData* sample = nullptr;
        int stream = -1;                 // DiskStreamer slot (sempl se cita sa diska), -1 = ceo u RAM-u
        juce::uint32 streamToken = 0;
        int block = 0;
        int lane = -1, slot = 0;             // mono traka (-1 = poly)
        bool eqOn = false;
        int delaySamples = 0;            // Note-On Delay: toliko semplova cuti i stoji na pocetku
        float drive = 0.0f, driveNorm = 1.0f, lowBoost = 0.0f;   // Driver (Kronos): zasicenje + bas
        float lbL = 0.0f, lbR = 0.0f, lbCoef = 0.0f;
        Biquad eq[3];
        int oscTranspose = 0;
        double pos = 0.0, inc = 1.0;
        double pitch = 60.0, pitchTarget = 60.0, glideCoef16 = 0.0;
        double rootKey = 60.0, tune = 0.0;     // tune u polutonovima
        bool fixedPitch = false, loop = false;
        double keySlope = 1.0;
        bool fmOn = false;                   // FM glas (bez sempla)
        double fmHz = 261.6;
        Fm::Voice fm;
        std::shared_ptr<const Fm::Patch> fmPatch;
        bool vlOn = false;                   // VL glas (fizicki model)
        Vl::Voice vl;
        std::shared_ptr<const Vl::Patch> vlPatch;
        int loopStart = 0, loopEnd = 0, endPos = 0, loopXfade = 0;
        float vel01 = 1.0f, velGain = 1.0f, gainL = 1.0f, gainR = 1.0f, ampMod = 1.0f;
        bool fadingOut = false;
        float fadeGain = 1.0f;
        float fadeOutStep = 0.0f;            // > 0: sporo pretapanje (legato crossfade), inace 6 ms
        float fadeIn = 1.0f, fadeInStep = 0.0f;   // legato crossfade: novi ton ulazi
        juce::uint64 age = 0;
        KronosEnv ampEnv, pitchEnv, filterEnv;
        double vibPhase = 0.0, fltPhase = 0.0, time = 0.0;
        int ctrl = 0;
        bool filterOn = false, cascade = false;
        juce::dsp::StateVariableTPTFilter<float> filter, filter2;
    };

    Voice* findFreeVoice();
    // blockMask: koji OSC-ovi sviraju; keyOff: samo keyoff MS-ovi
    void startVoices (int id, int pitch, int velocity, double startPitch, bool glide,
                      juce::uint32 blockMask, juce::uint32 serial, int lane, int slot, bool keyOff = false,
                      float legatoLevel = -1.0f);   // >= 0: legato crossfade (bez attack-a, od ovog nivoa envelope)
    void releaseVoice (Voice& v);
    void renderFmVoice (Voice& v, float* outL, float* outR, int num, float fadeStep);
    void updateModulation (Voice& v);
    void setGlide (Voice& v, double fromPitch, bool glide);
    struct MonoLane
    {
        std::vector<int> held;
        int key = -1;
        juce::uint32 serial = 0;
        double lastPitch = -1.0;
        int artic = -1;                      // OSC koji trenutno svira u grupi prelaza
    };
    static constexpr int numLanes = kNumBlocks + 1;   // svaki OSC + grupa prelaza
    static constexpr int articLane = kNumBlocks;

    int chooseMonoKey (const MonoLane&) const;
    void laneNoteOn (int lane, int slot, int id);
    void laneNoteOff (int lane, int slot, int id, bool sustainOn);
    void soundMono (int lane, int slot, int id, bool transition);
    int articDefault() const;
    bool laneHasRange (int lane, int pitch) const;
    void noteOffInternal (int id);

    static constexpr int maxVoices = 128;
    std::array<Voice, maxVoices> voices;

    const PartRuntime* cfg = nullptr;
    double sampleRate = 44100.0;
    float breath = 0.0f, rawExpression = 1.0f;
    struct VlPool { std::array<Vl::State, 6> st; double sr = 0.0; };
    std::unique_ptr<VlPool> vlPoolOwner;
    std::atomic<VlPool*> vlPool { nullptr };
    Vl::State* acquireVlState();
    void renderVlVoice (Voice& v, float* outL, float* outR, int num, float fadeStep);
    juce::uint64 ageCounter = 0;
    juce::uint32 serialCounter = 0;
    MonoLane lanes[numLanes][3];
    std::array<juce::uint32, numIds> polyMaskOf {};    // koji OSC-ovi su svirali poly za ovaj id
    bool sustainDown = false;
    float bend = 0.0f, modWheel = 0.0f, aftertouch = 0.0f;
    float vecX = 0.0f, vecY = 0.0f, expression = 1.0f;
    double lastPitch = -1.0;
    int polyHeld = 0;
    std::array<int, numIds> idPitch {}, idVel {};

    juce::AudioBuffer<float> partBuffer;
    InsertFx insertFx;
    Biquad eqLow, eqMid, eqHigh;
    float eqLast[4] { 999.0f, 999.0f, 999.0f, 999.0f };
    juce::SmoothedValue<float> gainL, gainR;
};

#pragma once
#include <JuceHeader.h>
#include "SampleLibrary.h"

// Citanje semplova sa diska dok se svira (za velike biblioteke): u RAM-u je samo pocetak sempla (preload),
// ostatak cita pozadinska nit u prsten-bafer svakog glasa. Audio nit nikad ne ceka disk:
// ako podatak jos nije stigao, taj deo je tisina (broji se kao "underrun").
class DiskStreamer : private juce::Thread
{
public:
    static DiskStreamer& get();

    static constexpr int numSlots = 192;
    static constexpr int ringFrames = 1 << 15;   // ~0.7 s na 48 kHz po glasu

    // audio nit: zauzmi slot za ovaj sempl (od preload-a nadalje); vraca -1 ako nema slobodnog
    int acquire (const SamplePtr& s, juce::uint32& token);
    void release (int slot, juce::uint32 token);
    // audio nit: gde je glas sada (da nit ne prepise ono sto se jos cita)
    void setReadPos (int slot, juce::uint32 token, int frame);
    // audio nit: jedan frame iz prstena (ch 0/1); false = jos nije procitano sa diska
    inline bool fetch (int slot, juce::uint32 token, int frame, float& l, float& r) const noexcept
    {
        const auto& s = slots[(size_t) slot];
        if (s.token.load (std::memory_order_relaxed) != token || s.state.load (std::memory_order_acquire) != 2) return false;
        if (frame < s.base || frame >= s.written.load (std::memory_order_acquire) || frame < s.written.load (std::memory_order_relaxed) - ringFrames + 8) return false;
        const auto idx = (size_t) ((frame - s.base) & (ringFrames - 1));
        l = (float) s.ringL[idx] * s.scale;
        r = (float) (s.mono ? s.ringL[idx] : s.ringR[idx]) * s.scale;
        return true;
    }

    int getUnderruns() const { return underruns.load(); }
    void countUnderrun() noexcept { underruns.fetch_add (1, std::memory_order_relaxed); }
    int getActiveStreams() const;
    juce::String debugSlot (int i) const { auto& s = slots[(size_t) i]; return juce::String (s.state.load()) + " base " + juce::String (s.base) + " written " + juce::String (s.written.load()) + " read " + juce::String (s.readPos.load()) + " total " + juce::String (s.total); }

private:
    DiskStreamer();
    ~DiskStreamer() override;
    void run() override;
    bool fill (int index);

    struct Slot
    {
        std::atomic<int> state { 0 };            // 0 slobodan, 1 trazen (audio), 2 radi, 3 pusten (audio) -> nit ga oslobadja
        std::atomic<juce::uint32> token { 0 };
        std::atomic<int> written { 0 };          // procitano do (ekskluzivno), apsolutni frame sempla
        std::atomic<int> readPos { 0 };
        std::atomic<juce::uint32> lastTouch { 0 };
        SamplePtr sample;                        // drzi ga nit dok cita (oslobadja se na niti, ne na audio niti)
        SamplePtr pending;                       // postavlja audio nit pri acquire
        int base = 0, total = 0;
        bool mono = false;
        float scale = 1.0f / 32767.0f;
        std::vector<juce::int16> ringL, ringR;
    };
    std::array<Slot, numSlots> slots;
    std::atomic<juce::uint32> nextToken { 1 };
    std::atomic<int> underruns { 0 };

    // otvoreni fajlovi (samo na niti)
    juce::AudioFormatManager formats;
    struct OpenFile { juce::String path; std::unique_ptr<juce::AudioFormatReader> reader; juce::uint32 used = 0; };
    std::vector<OpenFile> open;
    juce::AudioFormatReader* readerFor (const juce::File&);
    juce::AudioBuffer<float> tmp;
    juce::uint32 tick = 0;
};

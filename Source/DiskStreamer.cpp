#include "DiskStreamer.h"

DiskStreamer& DiskStreamer::get()
{
    static DiskStreamer instance;
    return instance;
}

DiskStreamer::DiskStreamer() : juce::Thread ("Pikaso disk streaming")
{
    formats.registerBasicFormats();
    for (auto& s : slots)
    {
        s.ringL.assign ((size_t) ringFrames, 0);
        s.ringR.assign ((size_t) ringFrames, 0);
    }
    tmp.setSize (2, 8192);
    startThread (juce::Thread::Priority::highest);
}

DiskStreamer::~DiskStreamer()
{
    stopThread (2000);
}

int DiskStreamer::acquire (const SamplePtr& s, juce::uint32& token)
{
    if (s == nullptr || ! s->streamed) return -1;
    for (int i = 0; i < numSlots; ++i)
    {
        auto& sl = slots[(size_t) i];
        int expected = 0;
        if (! sl.state.compare_exchange_strong (expected, -1)) continue;   // -1: audio nit ga upravo puni
        token = nextToken.fetch_add (1);
        sl.pending = s;                 // samo brojac reference (bez alokacije)
        sl.readPos.store (0);
        sl.written.store (0);
        sl.lastTouch.store (juce::Time::getMillisecondCounter());
        sl.token.store (token);
        sl.state.store (1, std::memory_order_release);
        return i;
    }
    return -1;
}

void DiskStreamer::release (int slot, juce::uint32 token)
{
    if (slot < 0 || slot >= numSlots) return;
    auto& sl = slots[(size_t) slot];
    if (sl.token.load() != token) return;
    int st = sl.state.load();
    if (st == 1 || st == 2) sl.state.compare_exchange_strong (st, 3);
}

void DiskStreamer::setReadPos (int slot, juce::uint32 token, int frame)
{
    if (slot < 0 || slot >= numSlots) return;
    auto& sl = slots[(size_t) slot];
    if (sl.token.load (std::memory_order_relaxed) != token) return;
    sl.readPos.store (frame, std::memory_order_relaxed);
    sl.lastTouch.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

int DiskStreamer::getActiveStreams() const
{
    int n = 0;
    for (auto& s : slots) n += s.state.load() == 2 ? 1 : 0;
    return n;
}

juce::AudioFormatReader* DiskStreamer::readerFor (const juce::File& f)
{
    const auto path = f.getFullPathName();
    for (auto& o : open)
        if (o.path == path) { o.used = ++tick; return o.reader.get(); }
    std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
    if (r == nullptr) return nullptr;
    if (open.size() >= 48)
    {
        auto it = std::min_element (open.begin(), open.end(), [] (const OpenFile& a, const OpenFile& b) { return a.used < b.used; });
        open.erase (it);
    }
    open.push_back ({ path, std::move (r), ++tick });
    return open.back().reader.get();
}

// jedan korak punjenja slota; true = jos ima posla
bool DiskStreamer::fill (int index)
{
    auto& sl = slots[(size_t) index];
    const int written = sl.written.load (std::memory_order_relaxed);
    if (written >= sl.total) return false;
    const int limit = juce::jmin (sl.total, juce::jmax (sl.base, sl.readPos.load (std::memory_order_relaxed)) + ringFrames - 64);
    const int avail = limit - written;
    if (avail <= 0 || (avail < 256 && limit < sl.total)) return false;   // dovoljno je unapred
    // ne preko kraja prstena u jednom potezu
    const int idx = (written - sl.base) & (ringFrames - 1);
    const int n = juce::jmin (avail, tmp.getNumSamples(), ringFrames - idx);
    auto* reader = readerFor (sl.sample->file);
    if (reader == nullptr) { sl.written.store (sl.total); return false; }
    tmp.clear (0, n);
    reader->read (&tmp, 0, n, (juce::int64) written, true, reader->numChannels > 1);
    const float* L = tmp.getReadPointer (0);
    const float* R = reader->numChannels > 1 ? tmp.getReadPointer (1) : L;
    const float inv = 1.0f / sl.scale;
    for (int i = 0; i < n; ++i)
    {
        sl.ringL[(size_t) (idx + i)] = (juce::int16) juce::jlimit (-32767, 32767, (int) std::lround (L[i] * inv));
        if (! sl.mono) sl.ringR[(size_t) (idx + i)] = (juce::int16) juce::jlimit (-32767, 32767, (int) std::lround (R[i] * inv));
    }
    sl.written.store (written + n, std::memory_order_release);
    return true;
}

void DiskStreamer::run()
{
    while (! threadShouldExit())
    {
        bool busy = false;
        const auto now = juce::Time::getMillisecondCounter();
        for (int i = 0; i < numSlots; ++i)
        {
            auto& sl = slots[(size_t) i];
            const int st = sl.state.load (std::memory_order_acquire);
            if (st == 1)
            {
                sl.sample = std::move (sl.pending);
                sl.pending.reset();
                if (sl.sample == nullptr) { sl.state.store (0); continue; }
                sl.base = sl.sample->residentFrames();
                sl.total = sl.sample->numFrames;
                sl.mono = sl.sample->isMono();
                sl.scale = sl.sample->pcmScale;
                sl.written.store (sl.base, std::memory_order_release);
                int expected = 1;
                if (! sl.state.compare_exchange_strong (expected, 2)) continue;   // vec pusten: ide na 3
                busy = true;
            }
            else if (st == 3 || (st == 2 && (int) (now - sl.lastTouch.load()) > 3000))
            {
                // pusten (ili glas nestao bez javljanja): oslobodi
                sl.sample.reset();
                sl.pending.reset();
                sl.token.store (0);
                sl.state.store (0, std::memory_order_release);
            }
            else if (st == 2)
            {
                // najvise 4 komada po prolazu da svi glasovi dobiju svoje
                for (int k = 0; k < 4 && fill (i); ++k) busy = true;
            }
        }
        if (! busy) wait (2);
    }
}

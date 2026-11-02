#pragma once
#include <JuceHeader.h>

// Automatsko trazenje loop-a (koristi ga Live Sampler i Kronos Autosampler).
// Trazi deo zvuka koji drzi (posle attack-a, pre puštanja tastera), pa u njemu par tacaka
// gde se talas najbolje poklapa. Crossfade (pretapanje) pred krajem loop-a skriva spoj.
namespace AutoLoop
{
    struct Settings
    {
        double minLoopSec = 0.3;        // najkraci loop
        double maxLoopSec = 3.0;        // najduzi loop
        double crossfadeSec = 0.08;     // pretapanje pred krajem loop-a
        float sustainDb = -6.0f;        // deo koji drzi: glasniji od (vrh + sustainDb)
        float maxDecayDb = 3.0f;        // ako zvuk izmedju pocetka i kraja loop-a opadne vise od ovoga: nema loop-a
        bool allowDecay = false;
    };

    struct Result
    {
        bool found = false;
        int start = 0, end = 0;         // end je ekskluzivan
        int crossfade = 0;              // u semplovima
        float quality = 0.0f;           // 1 = savrseno poklapanje
        juce::String reason;
    };

    inline Result find (const juce::AudioBuffer<float>& buf, int length, double sampleRate, const Settings& s)
    {
        Result r;
        const int len = juce::jmin (length, buf.getNumSamples());
        if (len < (int) (sampleRate * 0.3)) { r.reason = "prekratko"; return r; }

        // mono signal
        std::vector<float> m ((size_t) len);
        const float* L = buf.getReadPointer (0);
        const float* R = buf.getReadPointer (juce::jmin (1, buf.getNumChannels() - 1));
        for (int i = 0; i < len; ++i) m[(size_t) i] = 0.5f * (L[i] + R[i]);

        // RMS po blokovima
        const int blk = 512;
        const int nb = len / blk;
        std::vector<float> env ((size_t) nb);
        float peak = 0.0f;
        for (int b = 0; b < nb; ++b)
        {
            double e = 0;
            for (int i = 0; i < blk; ++i) e += m[(size_t) (b * blk + i)] * m[(size_t) (b * blk + i)];
            env[(size_t) b] = (float) std::sqrt (e / blk);
            peak = juce::jmax (peak, env[(size_t) b]);
        }
        if (peak < 1.0e-5f) { r.reason = "tisina"; return r; }

        const float thr = peak * juce::Decibels::decibelsToGain (s.sustainDb);
        int peakBlock = 0;
        for (int b = 0; b < nb; ++b) if (env[(size_t) b] >= peak * 0.7f) { peakBlock = b; break; }
        int lastBlock = nb - 1;
        while (lastBlock > peakBlock && env[(size_t) lastBlock] < thr) --lastBlock;

        const int regionStart = (peakBlock * blk) + (int) (sampleRate * 0.15);
        const int regionEnd = lastBlock * blk - (int) (sampleRate * 0.1);
        const int minLen = (int) (s.minLoopSec * sampleRate), maxLen = (int) (s.maxLoopSec * sampleRate);
        if (regionEnd - regionStart < minLen + 1024) { r.reason = "zvuk ne drzi dovoljno dugo"; return r; }

        auto risingZero = [&m] (int i) { return m[(size_t) i - 1] < 0.0f && m[(size_t) i] >= 0.0f; };

        // kraj loop-a: rastuci prolaz kroz nulu blizu kraja dela koji drzi
        int end = regionEnd;
        while (end > regionStart + minLen && ! risingZero (end)) --end;

        // poredjenje talasa oko dve tacke (pre i posle), normalizovano energijom
        const int W = juce::jmin (2048, minLen / 2);
        auto score = [&] (int a, int b)
        {
            double err = 0, en = 0;
            for (int k = -W / 2; k < W / 2; k += 2)
            {
                const float x = m[(size_t) (a + k)], y = m[(size_t) (b + k)];
                err += (x - y) * (x - y);
                en += x * x + y * y;
            }
            return en > 0 ? 1.0 - err / en : 0.0;
        };

        const int lo = juce::jmax (regionStart + W, end - maxLen), hi = end - minLen;
        if (hi <= lo || end + W / 2 >= len) { r.reason = "nema mesta za loop"; return r; }

        // grubo: rastuci prolazi kroz nulu (najvise ~4000 kandidata), pa fino oko najboljeg
        std::vector<int> cands;
        for (int i = lo; i < hi; ++i) if (risingZero (i)) cands.push_back (i);
        const size_t stride = juce::jmax ((size_t) 1, cands.size() / 4000);
        double best = -1.0;
        int bestStart = lo;
        for (size_t c = 0; c < cands.size(); c += stride)
        {
            const double q = score (cands[c], end);
            if (q > best) { best = q; bestStart = cands[c]; }
        }
        for (int i = juce::jmax (lo, bestStart - 300); i < juce::jmin (hi, bestStart + 300); ++i)
        {
            const double q = score (i, end);
            if (q > best) { best = q; bestStart = i; }
        }

        // zvuk koji opada (klavir, gitara...) nema smislen loop
        auto rmsAt = [&] (int pos)
        {
            double e = 0; const int w = juce::jmin (4096, (int) (sampleRate * 0.05));
            for (int k = 0; k < w; ++k) e += m[(size_t) (pos - w + k)] * m[(size_t) (pos - w + k)];
            return std::sqrt (e / w) + 1.0e-9;
        };
        const double dropDb = 20.0 * std::log10 (rmsAt (bestStart) / rmsAt (end));
        if (! s.allowDecay && std::abs (dropDb) > s.maxDecayDb)
        {
            r.reason = "zvuk opada (" + juce::String (dropDb, 1) + " dB), nema sustain";
            return r;
        }

        r.found = true;
        r.start = bestStart;
        r.end = end;
        r.quality = (float) juce::jlimit (0.0, 1.0, best);
        r.crossfade = juce::jmin ((int) (s.crossfadeSec * sampleRate), (r.end - r.start) / 2, r.start);
        return r;
    }

    // pretapanje upisano u sempl (za nove snimke iz autosamplera):
    // kraj loop-a postepeno prelazi u ono sto je pred pocetkom loop-a, pa je spoj gladak
    inline void bakeCrossfade (juce::AudioBuffer<float>& buf, int start, int end, int xf)
    {
        if (xf <= 0 || start - xf < 0 || end > buf.getNumSamples()) return;
        for (int c = 0; c < buf.getNumChannels(); ++c)
        {
            auto* d = buf.getWritePointer (c);
            for (int i = 0; i < xf; ++i)
            {
                const float w = (float) i / (float) xf;
                d[end - xf + i] = d[end - xf + i] * (1.0f - w) + d[start - xf + i] * w;   // linearno: tacke se vec poklapaju
            }
        }
    }
}

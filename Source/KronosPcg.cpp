#include "KronosPcg.h"

namespace Kronos
{
    bool Program::usesUser() const
    {
        for (auto& o : osc)
            for (auto& z : o)
                if (z.user) return true;
        return false;
    }

    int Program::firstUserZone (int oscIndex) const
    {
        for (int z = 0; z < 8; ++z)
            if (osc[(size_t) oscIndex][(size_t) z].user)
                return z;
        return -1;
    }

    juce::String exiEngineName (int a)
    {
        static const char* names[] = { "Off", "HD-1", "AL-1", "CX-3", "STR-1", "MS-20EX", "PolysixEX", "MOD-7", "SGX-2", "EP-1" };
        return a >= 0 && a < 10 ? juce::String (names[a]) : "EXi " + juce::String (a);
    }

    juce::String makeExiKey (const juce::uint8* program)
    {
        // FNV-1a 64 dva puta (razliciti pocetak) = 128-bitni otisak programa
        juce::uint64 a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
        for (int i = 0; i < programSize; ++i)
        {
            a = (a ^ program[i]) * 0x100000001b3ull;
            b = (b ^ program[programSize - 1 - i]) * 0x100000001b3ull;
        }
        return "exi:" + juce::String::toHexString ((juce::int64) a).paddedLeft ('0', 16) + juce::String::toHexString ((juce::int64) b).paddedLeft ('0', 16);
    }

    juce::String Program::engineText() const
    {
        if (! exi) return hd1 ? "HD-1" : "?";
        juce::String s = exiEngineName (exiAlg[0]);
        if (exiAlg[1] != 0) s << "+" << exiEngineName (exiAlg[1]);
        return s;
    }

    static juce::String letters (int i)
    {
        return juce::String::charToString ((juce::juce_wchar) ('A' + (i % 26)));
    }

    juce::String programBankName (juce::uint32 id)
    {
        const int hi = (int) (id >> 16), lo = (int) (id & 0xffff);
        if (hi == 0)
            return lo == 0x8000 ? juce::String ("GM") : "INT-" + letters (lo);
        if (hi == 2)
            return lo < 7 ? "USER-" + letters (lo) : "USER-" + letters (lo - 7) + letters (lo - 7);
        return "BANK " + juce::String::toHexString ((int) id);
    }

    juce::String combiBankName (juce::uint32 id)
    {
        const int hi = (int) (id >> 16), lo = (int) (id & 0xffff);
        if (hi == 0) return "INT-" + letters (lo);
        if (hi == 2) return "USER-" + letters (lo);
        return "BANK " + juce::String::toHexString ((int) id);
    }

    static juce::String readName (const juce::uint8* p)
    {
        juce::String s;
        for (int i = 0; i < 24 && p[i] != 0; ++i)
            s += (juce::juce_wchar) (p[i] < 128 ? p[i] : '?');
        return s.trim();
    }

    static juce::uint32 be32 (const juce::uint8* p)
    {
        return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | (juce::uint32) p[3];
    }

    // Mesta su utvrdjena test programima (T01-T14) napravljenim na Kronosu.
    // OSC2 parametri su na istim mestima + 466, osim raspona tastera.
    static void readParams (Program& p, const juce::uint8* d)
    {
        auto s8 = [] (juce::uint8 v) { return (int) (juce::int8) v; };
        p.doubleMode = (d[2558] & 0x01) != 0;
        p.drumKit    = (d[2558] & 0x02) != 0;
        p.busLR      = (d[2565] & 0x7f) == 0;
        p.mono       = (d[2558] & 0x80) != 0;
        p.legato     = (d[2559] & 0x10) != 0;
        p.priority   = (d[2559] >> 2) & 3;

        for (int o = 0; o < 2; ++o)
        {
            auto& q = p.params[(size_t) o];
            const auto* b = d + o * 466;
            q.keyLow  = d[2684 + o * 2];
            q.keyHigh = d[2685 + o * 2];
            q.velLow  = juce::jlimit (1, 127, (int) b[2972]);
            q.velHigh = juce::jlimit (1, 127, (int) b[2973]);
            for (int z = 0; z < 7; ++z)
            {
                q.zoneBottom[(size_t) z] = juce::jlimit (1, 127, (int) b[2950 + z * 3]);
                q.xfadeRange[(size_t) z] = juce::jlimit (0, 127, (int) b[2951 + z * 3]);
                q.xfadeCurve[(size_t) z] = juce::jlimit (0, 2, (int) b[2952 + z * 3]);
            }
            q.zoneBottom[7] = 1;
            q.transpose   = s8 (b[3013]);
            q.tuneCents   = (int) (juce::int16) (b[3014] | (b[3015] << 8));
            q.lfo1Freq    = b[2977];
            q.lfo1Offset  = s8 (b[2983]);
            q.lfo1Delay   = b[2984];
            q.lfo1Fade    = b[2985];
            q.octave      = juce::jlimit (-2, 1, s8 (b[3012]));
            q.pitchLfo1JsY = s8 (b[3030]);
            q.pitchEgInt  = s8 (b[3019]);
            q.pitchLfo1Int = s8 (b[3022]);
            q.portaOn        = b[3024] & 1;
            q.portaFingered  = (b[3024] >> 1) & 1;
            q.portaConstTime = (b[3024] >> 2) & 1;
            q.portaTime   = b[3025];
            q.bendUp      = s8 (b[3026]);
            q.bendDown    = s8 (b[3027]);
            q.filterRouting = b[3036];
            q.cutoff      = b[3047];
            q.resonance   = b[3048];
            q.filterEgInt = s8 (b[3056]);
            q.filterEgVel = s8 (b[3057]);
            for (int i = 0; i < 9; ++i) q.filterEg[(size_t) i] = s8 (b[3094 + i]);
            for (int i = 0; i < 8; ++i) q.ampEg[(size_t) i] = b[3151 + i];
            q.ampLevel    = b[3141];
            q.ampVelocity = s8 (b[3142]);
            q.send1       = b[3196];
            q.send2       = b[3197];
            q.pan         = b[3193];
        }
    }

    // Banka programa u timbre-u: 0-5 = INT-A..F, 17-30 = USER-A..GG
    static juce::uint32 timbreBank (int code)
    {
        if (code >= 0 && code <= 5)   return (juce::uint32) code;
        if (code >= 17 && code <= 30) return (2u << 16) | (juce::uint32) (code - 17);
        return 0xffffffff;
    }

    // Mesta utvrdjena test kombijima C01-C08
    static void readTimbres (Combi& c, const juce::uint8* d)
    {
        auto s8 = [] (juce::uint8 v) { return (int) (juce::int8) v; };
        c.return1 = juce::jlimit (0, 127, (int) d[1048]);
        c.category = d[4790] & 0x1f;
        c.return2 = juce::jlimit (0, 127, (int) d[1049]);
        for (int t = 0; t < 16; ++t)
        {
            const auto* b = d + timbresOffset + t * timbreSize;
            auto& tm = c.timbres[(size_t) t];
            tm.programNumber = b[0];
            tm.programBank = timbreBank (b[1]);
            tm.channel = (b[2] & 0x10) != 0 ? 0 : (b[2] & 0x0f) + 1;   // 0x10 = Gch (globalni kanal)
            tm.volume = b[5];
            tm.transpose = s8 (b[7]);
            tm.detuneCents = (int) (juce::int16) (b[8] | (b[9] << 8));
            tm.pan = b[14];
            tm.send1 = b[15];
            tm.send2 = b[16];
            tm.play = (b[34] & 0x80) == 0;
            tm.keyHigh = b[37];
            tm.keyLow = b[38];
            tm.velHigh = juce::jlimit (1, 127, (int) b[40]);
            tm.velLow = juce::jlimit (1, 127, (int) b[41]);
        }
    }

    const Program* Pcg::findProgram (juce::uint32 bankId, int number) const
    {
        for (auto& p : programs)
            if (p.bankId == bankId && p.number == number)
                return &p;
        return nullptr;
    }

    bool Pcg::load (const juce::File& f, juce::String& error)
    {
        programs.clear();
        combis.clear();
        drumKits = 0;
        file = f;

        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb))
        {
            error = "Ne mogu da procitam fajl";
            return false;
        }
        auto* d = (const juce::uint8*) mb.getData();
        const size_t len = mb.getSize();
        if (len < 32 || std::memcmp (d, "KORG", 4) != 0)
        {
            error = "Nije Korg PCG fajl";
            return false;
        }

        // blokovi: ID (4) + velicina (4, big-endian) + 4 bajta; sadrzaj sledi
        std::function<void (size_t, size_t)> walk = [&] (size_t start, size_t end)
        {
            size_t i = start;
            while (i + 12 <= end)
            {
                const juce::String id (juce::CharPointer_ASCII ((const char*) d + i), 4);
                const size_t size = be32 (d + i + 4);
                const size_t content = i + 12;
                if (content + size > end || size == 0)
                    break;

                if (id == "GLB1" && size > 4)
                {
                    globalChannel = (d[content + 2] & 0x0f) + 1;
                    // Global.txt: Combi Category 00..17 Name na 16800, po 24 bajta
                    if (size >= 16800 + 18 * 24)
                    {
                        combiCategories.clear();
                        for (int k = 0; k < 18; ++k)
                            combiCategories.add (readName (d + content + 16800 + k * 24).trim());
                    }
                }
                else if (id == "PCG1" || id == "PRG1" || id == "CMB1" || id == "DKT1")
                {
                    walk (content, content + size);
                }
                else if ((id == "PBK1" || id == "MBK1" || id == "CBK1" || id == "DBK1") && size >= 12)
                {
                    const juce::uint32 count = be32 (d + content);
                    const juce::uint32 itemSize = be32 (d + content + 4);
                    const juce::uint32 bankId = be32 (d + content + 8);
                    if (itemSize == 0 || 12 + (size_t) count * itemSize > size)
                    {
                        i = content + size;
                        continue;
                    }

                    for (juce::uint32 k = 0; k < count; ++k)
                    {
                        const auto* item = d + content + 12 + (size_t) k * itemSize;
                        if (id == "DBK1")
                        {
                            ++drumKits;
                            continue;
                        }
                        if (id == "CBK1")
                        {
                            Combi c;
                            c.bankId = bankId;
                            c.bank = combiBankName (bankId);
                            c.number = (int) k;
                            c.name = readName (item);
                            if (itemSize >= (juce::uint32) combiSize)
                                readTimbres (c, item);
                            combis.push_back (std::move (c));
                            continue;
                        }

                        Program p;
                        p.bankId = bankId;
                        p.bank = programBankName (bankId);
                        p.number = (int) k;
                        p.name = readName (item);
                        p.hd1 = id == "PBK1" && itemSize == (juce::uint32) programSize;
                        if (id == "MBK1" && itemSize == (juce::uint32) programSize && (item[2558] & 0x03) == 3)
                        {
                            p.exi = true;
                            p.exiAlg = { item[exi1AlgOffset], item[exi2AlgOffset] };
                            p.exiKey = makeExiKey (item);
                            p.mono = (item[2558] & 0x80) != 0;
                            p.legato = (item[2559] & 0x10) != 0;
                            p.priority = (item[2559] >> 2) & 3;
                            p.busLR = (item[2565] & 0x1f) == 0;
                            p.exiSend1 = item[2864];
                            p.exiSend2 = item[2865];
                        }
                        if (p.hd1)
                        {
                            readParams (p, item);
                            if (p.drumKit)
                            {
                                // drum kit program: zone ne nose multisempl nego broj drum kita
                                for (auto& o : p.osc)
                                    for (auto& z : o) { z.user = false; }
                                programs.push_back (std::move (p));
                                continue;
                            }
                            for (int o = 0; o < 2; ++o)
                            {
                                const int base = o == 0 ? osc1ZonesOffset : osc2ZonesOffset;
                                for (int z = 0; z < 8; ++z)
                                {
                                    const auto* zp = item + base + z * zoneSize;
                                    auto& zone = p.osc[(size_t) o][(size_t) z];
                                    const bool korg = std::memcmp (zp + 1, "KORG", 4) == 0;
                                    // Zone MS Type: 0 = Off, 1 = Multisample, 2 = Wave Sequence (Korg Prog_HD-1.txt).
                                    // Iskljucena zona moze da zadrzi stari UUID ("KORG...") pa se ne sme brojati.
                                    const int type = zp[0] & 0x03;
                                    zone.user = type == 1 && ! korg;
                                    zone.used = type != 0 && (zone.user || korg);
                                    zone.ms = zp[18] | (zp[19] << 8);
                                    zone.value = zp[20];
                                    if (zone.user)
                                        zone.uuid = juce::String::toHexString (zp + 1, 16, 0).toLowerCase();
                                    else if (korg && zone.used)
                                        zone.romKey = "rom:" + juce::String::toHexString (zp, 20, 0).toLowerCase();
                                }
                            }
                        }
                        programs.push_back (std::move (p));
                    }
                }
                i = content + size;
            }
        };

        walk (16, len);
        if (programs.empty() && combis.empty())
        {
            error = "U fajlu nema programa ni kombija";
            return false;
        }
        return true;
    }
}

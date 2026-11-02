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

                if (id == "PCG1" || id == "PRG1" || id == "CMB1" || id == "DKT1")
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
                            combis.push_back ({ bankId, combiBankName (bankId), (int) k, readName (item) });
                            continue;
                        }

                        Program p;
                        p.bankId = bankId;
                        p.bank = programBankName (bankId);
                        p.number = (int) k;
                        p.name = readName (item);
                        p.hd1 = id == "PBK1" && itemSize == (juce::uint32) programSize;
                        if (p.hd1)
                        {
                            for (int o = 0; o < 2; ++o)
                            {
                                const int base = o == 0 ? osc1ZonesOffset : osc2ZonesOffset;
                                for (int z = 0; z < 8; ++z)
                                {
                                    const auto* zp = item + base + z * zoneSize;
                                    auto& zone = p.osc[(size_t) o][(size_t) z];
                                    const bool korg = std::memcmp (zp + 1, "KORG", 4) == 0;
                                    zone.user = zp[0] == 1 && ! korg;
                                    zone.used = zone.user || korg;
                                    zone.ms = zp[18] | (zp[19] << 8);
                                    zone.value = zp[20];
                                    if (zone.user)
                                        zone.uuid = juce::String::toHexString (zp + 1, 16, 0).toLowerCase();
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

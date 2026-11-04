#include "SampleLibrary.h"
#include "Text.h"
#include <regex>

//==============================================================================
//  Instrument
//==============================================================================
const Zone* Instrument::findZone (int key, int velocity) const
{
    int count = 0;
    for (auto& z : zones)
        if (z.matches (key, velocity))
            ++count;

    if (count == 0)
        return nullptr;

    // vise zona na istom mestu = round robin
    int pick = count == 1 ? 0 : (int) (roundRobinCounter.fetch_add (1) % (unsigned) count);
    for (auto& z : zones)
        if (z.matches (key, velocity) && pick-- == 0)
            return &z;

    return nullptr;
}

size_t Instrument::memoryBytes() const
{
    size_t total = 0;
    for (auto& s : samples)
        total += (size_t) s->buffer.getNumSamples() * (size_t) s->buffer.getNumChannels() * sizeof (float);
    return total;
}

juce::File Instrument::getMapFile() const
{
    if (type == SourceType::WavFolder)
        return source.getChildFile ("LiveSampler-mapa.xml");
    return source.getSiblingFile (source.getFileName() + ".lsmap.xml");
}

std::shared_ptr<Instrument> Instrument::withZones (std::vector<Zone> newZones) const
{
    auto n = std::make_shared<Instrument>();
    n->name = name;
    n->type = type;
    n->source = source;
    n->samples = samples;
    n->warnings = warnings;
    n->fromMapFile = fromMapFile;
    n->zones = std::move (newZones);
    return n;
}

bool Instrument::saveMap() const
{
    juce::XmlElement root ("LiveSamplerMap");
    root.setAttribute ("version", 1);
    for (auto& z : zones)
    {
        if (z.sample < 0 || z.sample >= (int) samples.size())
            continue;
        auto* e = root.createNewChildElement ("Zone");
        e->setAttribute ("sample", samples[(size_t) z.sample]->id);
        e->setAttribute ("enabled", z.enabled ? 1 : 0);
        e->setAttribute ("lowKey", z.lowKey);
        e->setAttribute ("highKey", z.highKey);
        e->setAttribute ("lowVel", z.lowVel);
        e->setAttribute ("highVel", z.highVel);
        e->setAttribute ("root", z.rootKey);
        e->setAttribute ("tune", (double) z.tuneCents);
        e->setAttribute ("gain", (double) z.gainDb);
        e->setAttribute ("pan", (double) z.pan);
        e->setAttribute ("loop", z.loop ? 1 : 0);
        e->setAttribute ("fixedPitch", z.fixedPitch ? 1 : 0);
        if (z.sampleStart > 0) e->setAttribute ("sampleStart", z.sampleStart);
        if (z.sampleEnd > 0) e->setAttribute ("sampleEnd", z.sampleEnd);
        if (z.loopXfade > 0) e->setAttribute ("loopXfade", z.loopXfade);
        if (z.hasOwnLoop())
        {
            e->setAttribute ("loopStart", z.loopStart);
            e->setAttribute ("loopEnd", z.loopEnd);
        }
    }
    return root.writeTo (getMapFile());
}

juce::String sourceTypeName (SourceType t)
{
    switch (t)
    {
        case SourceType::Korg:    return "Korg KMP";
        case SourceType::Kontakt:   return "Kontakt NKI";
        case SourceType::WavFolder: break;
    }
    return "WAV folder";
}

//==============================================================================
//  Pomocne funkcije
//==============================================================================
static bool isAudioFile (const juce::File& f)
{
    auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}

static void trimLeadingSilence (SampleData& sd)
{
    const int len = sd.buffer.getNumSamples();
    const float thr = 0.0003f;
    int first = 0;
    auto* l = sd.buffer.getReadPointer (0);
    auto* r = sd.buffer.getReadPointer (1);
    while (first < len && std::abs (l[first]) < thr && std::abs (r[first]) < thr)
        ++first;
    first = juce::jmax (0, first - 32);
    if (sd.hasLoop)
        first = juce::jmin (first, sd.loopStart);
    sd.startOffset = first < len - 64 ? first : 0;
}

// pronalazi fajl bez obzira na velika/mala slova
static juce::File findChildIgnoringCase (const juce::File& dir, const juce::String& name)
{
    auto direct = dir.getChildFile (name);
    if (direct.existsAsFile())
        return direct;

    if (dir.isDirectory())
        for (const auto& e : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
            if (e.getFile().getFileName().equalsIgnoreCase (name))
                return e.getFile();

    return {};
}

//==============================================================================
SampleLibrary::SampleLibrary()
{
    formatManager.registerBasicFormats();
}

juce::String SampleLibrary::rootLabel (const juce::File& r)
{
    auto n = r.getFileName();
    if (n.isEmpty())   // ceo disk / USB stick, npr. "E:\"
        n = r.getFullPathName().trimCharactersAtEnd ("\\/");
    return n.isEmpty() ? juce::String ("Disk") : n;
}

void SampleLibrary::scan (const juce::Array<juce::File>& rootFolders)
{
    const juce::ScopedLock sl (lock);
    roots = rootFolders;
    names.clear();
    entries.clear();
    kronosMs.clear();
    kronosTags.clear();

    for (auto& r : roots)
        if (r.isDirectory())
            scanRoot (r);

    names.sortNatural();
}

juce::String SampleLibrary::findKronosRom (const juce::String& romKey) const
{
    const juce::ScopedLock sl (lock);
    auto it = kronosMs.find (romKey.toLowerCase());
    return it != kronosMs.end() ? it->second : juce::String();
}

juce::String SampleLibrary::kronosTag (const juce::String& key, const juce::String& name) const
{
    const juce::ScopedLock sl (lock);
    auto it = kronosTags.find (key.toLowerCase());
    return it != kronosTags.end() ? it->second.getValue (name, {}) : juce::String();
}

juce::String SampleLibrary::findKronosMultisample (const juce::String& uuid, int index) const
{
    const juce::ScopedLock sl (lock);
    auto it = kronosMs.find (uuid.toLowerCase() + ":" + juce::String (index));
    return it != kronosMs.end() ? it->second : juce::String();
}

void SampleLibrary::scanRoot (const juce::File& root)
{

    std::set<juce::File> wavDirs;
    juce::Array<juce::File> kmpFiles, nkiFiles, kontaktFileDirs, kscFiles;

    for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
    {
        const auto& f = entry.getFile();
        const auto ext = f.getFileExtension().toLowerCase();
        if (isAudioFile (f))      wavDirs.insert (f.getParentDirectory());
        else if (ext == ".kmp")   kmpFiles.add (f);
        else if (ext == ".ksc")   kscFiles.add (f);
        else if (ext == ".nki")   nkiFiles.add (f);
        if (ext == ".nki" || ext == ".nkm")
            kontaktFileDirs.addIfNotAlreadyThere (f.getParentDirectory());
    }

    // folderi sa semplovima Kontakt biblioteka ne treba da se vide kao posebni instrumenti
    juce::Array<juce::File> kontaktRoots;
    for (auto& nki : nkiFiles)
    {
        auto d = nki.getParentDirectory();
        if (d.getFileName().equalsIgnoreCase ("Instruments"))
            d = d.getParentDirectory();
        if (d != root)
            kontaktRoots.addIfNotAlreadyThere (d);
    }

    const auto label = rootLabel (root);
    auto rel = [&root, &label] (const juce::File& f)
    {
        return f == root ? label : label + "/" + f.getRelativePathFrom (root).replaceCharacter ('\\', '/');
    };

    auto add = [this] (juce::String name, SourceType type, const juce::File& f, const juce::String& suffix)
    {
        if (entries.count (name) > 0)
            name << " (" << suffix << ")";
        entries[name] = { type, f };
        names.add (name);
    };

    for (auto& d : wavDirs)
    {
        bool insideKontakt = false;
        for (auto& k : kontaktRoots)
            if (d == k || d.isAChildOf (k))
                insideKontakt = true;

        // "<ime> Samples" / "Samples" folderi pored Kontakt instrumenata
        if (d.getFileName().endsWithIgnoreCase ("Samples"))
        {
            const auto parent = d.getParentDirectory();
            for (auto& k : kontaktFileDirs)
                if (k == parent || k.isAChildOf (parent))
                    insideKontakt = true;
        }
        for (auto a = d.getParentDirectory(); a != root && a.isAChildOf (root); a = a.getParentDirectory())
            if (a.getFileName().endsWithIgnoreCase ("Samples") && kontaktFileDirs.contains (a.getParentDirectory()))
                insideKontakt = true;
        if (! insideKontakt)
        {
            add (rel (d), SourceType::WavFolder, d, "WAV");
            // presemplovan Kronos ROM multisempl (Kronos Autosampler upisuje kronos-ms.txt)
            const auto tag = d.getChildFile ("kronos-ms.txt");
            if (tag.existsAsFile())
            {
                juce::String key;
                juce::StringPairArray vals;
                for (auto& l : juce::StringArray::fromLines (tag.loadFileAsString()))
                {
                    const auto t = l.trim();
                    if (t.startsWith ("rom:") || t.startsWith ("exi:"))
                    {
                        key = t.toLowerCase();
                        kronosMs[key] = names[names.size() - 1];
                    }
                    else if (t.containsChar ('='))
                        vals.set (t.upToFirstOccurrenceOf ("=", false, false).trim(), t.fromFirstOccurrenceOf ("=", false, false).trim());
                }
                if (key.isNotEmpty() && vals.size() > 0)
                    kronosTags[key] = vals;
            }
        }
    }

    // Korg Kronos KSC kolekcije: multisemplovi dobijaju broj i ime kao na Kronosu
    std::set<juce::File> claimed;
    for (auto& ksc : kscFiles)
    {
        if (ksc.getFileNameWithoutExtension().endsWithIgnoreCase ("_UserBank"))
            continue;
        const auto lines = juce::StringArray::fromLines (ksc.loadFileAsString());
        if (lines.isEmpty() || ! lines[0].startsWith ("#KORG Script"))
            continue;

        juce::String uuid;
        juce::StringArray kmpNames;
        for (auto& l : lines)
        {
            const auto t = l.trim();
            if (t.startsWith ("#uuid:")) uuid = t.fromFirstOccurrenceOf ("#uuid:", false, false).removeCharacters ("-").toLowerCase();
            else if (! t.startsWith ("#") && t.endsWithIgnoreCase (".KMP")) kmpNames.add (t);
        }
        if (kmpNames.isEmpty())
            continue;

        // broj i ime multisempla iz "<ime>_UserBank.KSC" (istim redom kao KMP fajlovi)
        std::vector<std::pair<int, juce::String>> msInfo;
        const auto bankFile = ksc.getSiblingFile (ksc.getFileNameWithoutExtension() + "_UserBank.KSC");
        for (auto& l : juce::StringArray::fromLines (bankFile.loadFileAsString()))
        {
            const int msPos = l.indexOf (".MS");
            if (! l.startsWith ("#>>") || msPos < 0) continue;
            auto rest = l.substring (msPos + 3);
            const int idx = rest.upToFirstOccurrenceOf (".", false, false).getIntValue();
            auto parts = juce::StringArray::fromTokens (rest, ".", {});
            juce::String nm;
            for (int k = 3; k < parts.size(); ++k) nm << (k > 3 ? "." : "") << parts[k];
            msInfo.push_back ({ idx, nm.trim() });
        }

        const auto folder = ksc.getSiblingFile (ksc.getFileNameWithoutExtension());
        const auto base = rel (ksc.withFileExtension ({}));
        for (int k = 0; k < kmpNames.size(); ++k)
        {
            auto kmp = findChildIgnoringCase (folder, kmpNames[k]);
            if (! kmp.existsAsFile())
                continue;
            claimed.insert (kmp);

            const bool known = k < (int) msInfo.size();
            const int idx = known ? msInfo[(size_t) k].first : k;
            auto nm = known ? msInfo[(size_t) k].second : kmp.getFileNameWithoutExtension();
            const auto entryName = base + "/MS" + juce::String (idx).paddedLeft ('0', 4) + " " + nm.replaceCharacter ('/', '-');
            add (entryName, SourceType::Korg, kmp, "KMP");
            if (uuid.isNotEmpty())
                kronosMs[uuid + ":" + juce::String (idx)] = names[names.size() - 1];
        }
    }

    for (auto& f : kmpFiles)
        if (claimed.count (f) == 0)
            add (rel (f.withFileExtension ({})), SourceType::Korg, f, "KMP");
    for (auto& f : nkiFiles)
        add (rel (f.withFileExtension ({})), SourceType::Kontakt, f, "NKI");
}

//==============================================================================
bool SampleLibrary::parseFileName (const juce::String& fileNameWithoutExt, bool c3Is60,
                                   int& note, int& velocityLayer)
{
    std::string s = fileNameWithoutExt.toLowerCase().toStdString();
    note = -1;
    velocityLayer = 0;

    // 1) velocity sloj: v1, vel2, velocity_3, layer4, ili dinamika pp/p/mp/mf/f/ff
    {
        static const std::regex velRe ("(^|[^a-z])((?:velocity|velo|vel|layer|v)[ _-]?([0-9]{1,3}))(?![0-9])");
        std::smatch m;
        std::string rest = s;
        bool found = false;
        for (auto it = std::sregex_iterator (s.begin(), s.end(), velRe); it != std::sregex_iterator(); ++it)
        {
            m = *it;
            velocityLayer = std::stoi (m[3].str());
            found = true;
            // izbaci samo "v3" deo iz stringa da se broj ne pomesa sa notom
            auto pos = (size_t) (m[2].first - s.cbegin());
            rest = s.substr (0, pos) + " " + s.substr (pos + (size_t) m[2].length());
        }

        if (! found)
        {
            // dinamika kao posebna rec; "f#3" ili "f-1" je nota, ne forte
            static const std::regex dynRe ("(?:^|[^a-z0-9])(ppp|pp|p|mp|mf|f|ff|fff)(?![a-z0-9#]|-[0-9])");
            static const std::map<std::string, int> dyn {
                { "ppp", 1 }, { "pp", 2 }, { "p", 3 }, { "mp", 4 },
                { "mf", 5 }, { "f", 6 }, { "ff", 7 }, { "fff", 8 } };

            for (auto it = std::sregex_iterator (s.begin(), s.end(), dynRe); it != std::sregex_iterator(); ++it)
                velocityLayer = dyn.at ((*it)[1].str());
        }
        s = rest;
    }

    // 2) ime note: C3, C#3, Db3, Cs3, F#-1 ... uzima se poslednje pojavljivanje
    {
        static const std::regex noteRe ("(?:^|[^a-z])([a-g])(#|b|s)?(-?[0-9])(?![0-9])");
        static const int semis[] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
        const int octaveOffset = c3Is60 ? 2 : 1;

        for (auto it = std::sregex_iterator (s.begin(), s.end(), noteRe); it != std::sregex_iterator(); ++it)
        {
            auto& m = *it;
            int semi = semis[m[1].str()[0] - 'a'];
            auto acc = m[2].str();
            if (acc == "#" || acc == "s") semi += 1;
            if (acc == "b") semi -= 1;
            int octave = std::stoi (m[3].str());
            int n = (octave + octaveOffset) * 12 + semi;
            if (n >= 0 && n <= 127)
                note = n;
        }
    }

    // 3) ako nema imena note, probaj MIDI broj (npr. "Piano_060")
    if (note < 0)
    {
        static const std::regex numRe ("(?:^|[^0-9])([0-9]{2,3})(?![0-9])");
        for (auto it = std::sregex_iterator (s.begin(), s.end(), numRe); it != std::sregex_iterator(); ++it)
        {
            int n = std::stoi ((*it)[1].str());
            if (n >= 0 && n <= 127)
                note = n;
        }
    }

    return note >= 0;
}

//==============================================================================
//  WAV / AIFF / FLAC
//==============================================================================
SamplePtr SampleLibrary::loadAudioFile (const juce::File& f, const juce::String& id, juce::StringArray& warnings)
{
    std::unique_ptr<juce::AudioFormatReader> reader;
    {
        const juce::ScopedLock sl (lock);
        reader.reset (formatManager.createReaderFor (f));
    }
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        warnings.add (T ("Cannot read: ") + f.getFileName());
        return nullptr;
    }

    auto sd = std::make_shared<SampleData>();
    sd->id = id;
    sd->displayName = f.getFileNameWithoutExtension();
    sd->sampleRate = reader->sampleRate;

    const int len = (int) juce::jmin ((juce::int64) std::numeric_limits<int>::max() / 2, reader->lengthInSamples);
    sd->buffer.setSize (2, len);
    reader->read (&sd->buffer, 0, len, 0, true, reader->numChannels > 1);
    if (reader->numChannels == 1)
        sd->buffer.copyFrom (1, 0, sd->buffer, 0, 0, len);

    // loop tacke i root nota iz WAV "smpl" bloka
    auto& meta = reader->metadataValues;
    if (meta.getValue ("NumSampleLoops", "0").getIntValue() > 0)
    {
        int ls = meta.getValue ("Loop0Start", "0").getIntValue();
        int le = meta.getValue ("Loop0End", "0").getIntValue() + 1;
        if (ls >= 0 && le > ls + 16 && le <= len)
        {
            sd->hasLoop = true;
            sd->loopStart = ls;
            sd->loopEnd = le;
        }
    }
    if (meta.containsKey ("MidiUnityNote"))
    {
        int r = meta.getValue ("MidiUnityNote", "-1").getIntValue();
        if (r >= 0 && r <= 127)
            sd->fileRootKey = r;
    }

    trimLeadingSilence (*sd);
    return sd;
}

void SampleLibrary::autoMapByName (Instrument& inst, const std::vector<int>& sampleIndices, bool c3Is60)
{
    struct Parsed { int sample; int note; int layer; };
    std::vector<Parsed> parsed;

    for (int idx : sampleIndices)
    {
        auto& s = inst.samples[(size_t) idx];
        int note = -1, layer = 0;
        if (! parseFileName (s->displayName, c3Is60, note, layer))
            note = s->fileRootKey;

        if (note < 0)
        {
            if (sampleIndices.size() == 1)
                note = 60; // jedan fajl bez note: svira na celoj klavijaturi
            else
            {
                inst.warnings.add ("No note in the file name: " + s->displayName);
                Zone z;
                z.sample = idx;
                z.enabled = false;  // vidi se u editoru, moze rucno da se mapira
                inst.zones.push_back (z);
                continue;
            }
        }
        parsed.push_back ({ idx, note, layer });
    }

    if (parsed.empty())
        return;

    std::vector<int> layers;
    for (auto& p : parsed)
        if (std::find (layers.begin(), layers.end(), p.layer) == layers.end())
            layers.push_back (p.layer);
    std::sort (layers.begin(), layers.end());

    // Ako su brojevi slojeva vece vrednosti (npr. v40 v80 v127), tretiraj ih kao gornju granicu jacine
    const bool layersAreVelocities = layers.size() > 1 && layers.back() > 16 && layers.back() <= 127;
    const int numLayers = (int) layers.size();

    for (int li = 0; li < numLayers; ++li)
    {
        int lowVel, highVel;
        if (layersAreVelocities)
        {
            lowVel = li == 0 ? 1 : layers[(size_t) li - 1] + 1;
            highVel = li == numLayers - 1 ? 127 : layers[(size_t) li];
        }
        else
        {
            lowVel = li == 0 ? 1 : (li * 128) / numLayers;
            highVel = li == numLayers - 1 ? 127 : ((li + 1) * 128) / numLayers - 1;
        }

        std::map<int, std::vector<int>> byRoot;
        for (auto& p : parsed)
            if (p.layer == layers[(size_t) li])
                byRoot[p.note].push_back (p.sample);

        std::vector<int> roots;
        for (auto& kv : byRoot)
            roots.push_back (kv.first);

        for (size_t i = 0; i < roots.size(); ++i)
        {
            for (int sampleIdx : byRoot[roots[i]])   // vise semplova = round robin
            {
                Zone z;
                z.sample = sampleIdx;
                z.rootKey = roots[i];
                z.lowKey  = i == 0 ? 0 : (roots[i - 1] + roots[i]) / 2 + 1;
                z.highKey = i == roots.size() - 1 ? 127 : (roots[i] + roots[i + 1]) / 2;
                z.lowVel = lowVel;
                z.highVel = highVel;
                inst.zones.push_back (z);
            }
        }
    }
}

void SampleLibrary::loadWavFolder (Instrument& inst, bool c3Is60)
{
    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (inst.source, false, "*", juce::File::findFiles))
        if (isAudioFile (e.getFile()))
            files.add (e.getFile());
    files.sort();

    std::vector<int> indices;
    for (auto& f : files)
        if (auto s = loadAudioFile (f, f.getFileName(), inst.warnings))
        {
            indices.push_back ((int) inst.samples.size());
            inst.samples.push_back (s);
        }

    autoMapByName (inst, indices, c3Is60);
}

//==============================================================================
//  Korg KMP / KSF  (Triton, Trinity, Karma, Kronos, M3, Oasys, Pa serija)
//==============================================================================
namespace
{
    struct Chunk { juce::String id; size_t offset = 0; size_t size = 0; };

    struct ChunkReader
    {
        const juce::uint8* d = nullptr;
        size_t len = 0;
        bool bigEndian = true;

        juce::uint32 u32 (size_t pos) const
        {
            if (pos + 4 > len) return 0;
            return bigEndian ? (juce::uint32) juce::ByteOrder::bigEndianInt (d + pos)
                             : (juce::uint32) juce::ByteOrder::littleEndianInt (d + pos);
        }

        std::vector<Chunk> readChunks()
        {
            std::vector<Chunk> out;
            size_t pos = 0;
            bool decided = false;
            while (pos + 8 <= len)
            {
                Chunk c;
                c.id = juce::String (juce::CharPointer_ASCII ((const char*) d + pos), 4);
                const size_t be = juce::ByteOrder::bigEndianInt (d + pos + 4);
                const size_t le = juce::ByteOrder::littleEndianInt (d + pos + 4);
                const size_t avail = len - pos - 8;

                size_t size;
                if (decided)                         size = bigEndian ? be : le;
                else if (be <= avail)              { size = be; bigEndian = true;  decided = true; }
                else if (le <= avail)              { size = le; bigEndian = false; decided = true; }
                else                                 size = avail;

                size = juce::jmin (size, avail);
                c.offset = pos + 8;
                c.size = size;
                out.push_back (c);
                pos += 8 + size;
            }
            return out;
        }

        static const Chunk* find (const std::vector<Chunk>& chunks, const char* id)
        {
            for (auto& c : chunks)
                if (c.id == id)
                    return &c;
            return nullptr;
        }

        juce::String text (size_t pos, size_t n) const
        {
            if (pos + n > len) return {};
            size_t end = 0;
            while (end < n && d[pos + end] != 0)
                ++end;
            juce::String s;
            for (size_t i = 0; i < end; ++i)
                s += (juce::juce_wchar) (d[pos + i] < 128 ? d[pos + i] : '?');
            return s.trim();
        }
    };
}

SamplePtr SampleLibrary::loadKsf (const juce::File& f, const juce::String& id, juce::StringArray& warnings, int depth)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb))
    {
        warnings.add (T ("Cannot read: ") + f.getFileName());
        return nullptr;
    }

    ChunkReader r { (const juce::uint8*) mb.getData(), mb.getSize() };
    auto chunks = r.readChunks();
    auto* smp = ChunkReader::find (chunks, "SMP1");
    auto* smd = ChunkReader::find (chunks, "SMD1");
    const bool be = r.bigEndian;

    // Korg SMP1: ime (16), banka (1), Start (3), Start2 (4), Loop Start (4), End (4).
    // SMD1 atributi: bit 7 = loop iskljucen (Loop Off), tada se svira od Start do End jednom.
    auto applyPoints = [&] (SampleData& sd, int frames, juce::uint8 attributes)
    {
        if (smp == nullptr || smp->size < 32) return false;
        const auto* sp = r.d + smp->offset;
        const int start = be ? ((sp[17] << 16) | (sp[18] << 8) | sp[19]) : ((sp[19] << 16) | (sp[18] << 8) | sp[17]);
        const int ls = (int) r.u32 (smp->offset + 24);
        const int le = juce::jmin ((int) r.u32 (smp->offset + 28) + 1, frames);
        const bool loopOff = (attributes & 0x80) != 0;
        sd.startOffset = start >= 0 && start < le - 16 ? start : 0;
        sd.endPoint = le > sd.startOffset + 16 ? le : frames;
        sd.hasLoop = false;
        if (! loopOff && ls >= 0 && le - ls >= 16)
        {
            sd.hasLoop = true;
            sd.loopStart = ls;
            sd.loopEnd = le;
        }
        return true;
    };

    // SMF1: ovaj sempl koristi zvuk iz drugog KSF fajla (Kronos tako deli isti zvuk izmedju zona),
    // a svoje su mu samo tacke (start, loop, end) iz SMP1
    if (auto* smf = ChunkReader::find (chunks, "SMF1"); smf != nullptr && smf->size > 0)
    {
        const auto refName = r.text (smf->offset, (int) smf->size).trim();
        const auto ref = f.getSiblingFile (refName);
        if (depth < 4 && refName.isNotEmpty() && ref != f && ref.existsAsFile())
        {
            juce::StringArray dummy;
            if (auto base = loadKsf (ref, id, dummy, depth + 1))
            {
                auto sd = std::make_shared<SampleData> (*base);
                sd->id = id;
                if (smp != nullptr && smp->size >= 16)
                    if (auto n = r.text (smp->offset, 16); n.isNotEmpty()) sd->displayName = n;
                const juce::uint8 attributes = smd != nullptr && smd->size >= 5 ? r.d[smd->offset + 4] : (juce::uint8) (base->hasLoop ? 0x00 : 0x80);
                applyPoints (*sd, sd->buffer.getNumSamples(), attributes);
                return sd;
            }
        }
        warnings.add (f.getFileName() + T (": points to ") + refName + T (", which does not exist"));
        return nullptr;
    }

    if (smd == nullptr)
    {
        warnings.add (f.getFileName() + T (": not a Korg KSF sample"));
        return nullptr;
    }
    if (smd->size < 12)
    {
        warnings.add (f.getFileName() + T (": damaged KSF"));
        return nullptr;
    }

    double rate = r.u32 (smd->offset);
    const juce::uint8 attributes = r.d[smd->offset + 4];
    const int channels = juce::jmax (1, (int) r.d[smd->offset + 6]);
    const int bits = (int) r.d[smd->offset + 7];
    const juce::uint32 points = r.u32 (smd->offset + 8);

    if (attributes & 0x10)
    {
        warnings.add (f.getFileName() + T (": compressed KSF, not supported"));
        return nullptr;
    }
    if (bits != 8 && bits != 16 && bits != 24)
    {
        warnings.add (f.getFileName() + ": " + juce::String (bits) + T ("-bit KSF is not supported"));
        return nullptr;
    }
    if (rate < 4000.0 || rate > 200000.0)
        rate = 48000.0;

    const int bytesPerSample = bits / 8;
    const size_t frameBytes = (size_t) (bytesPerSample * channels);
    const size_t dataOffset = smd->offset + 12;
    const int frames = (int) juce::jmin ((size_t) points, (smd->size - 12) / frameBytes);
    if (frames <= 0)
    {
        warnings.add (f.getFileName() + T (": empty sample"));
        return nullptr;
    }

    auto sd = std::make_shared<SampleData>();
    sd->id = id;
    sd->displayName = f.getFileNameWithoutExtension();
    if (smp != nullptr && smp->size >= 16)
    {
        auto n = r.text (smp->offset, 16);
        if (n.isNotEmpty()) sd->displayName = n;
    }
    sd->sampleRate = rate;
    sd->buffer.setSize (2, frames);

    for (int ch = 0; ch < 2; ++ch)
    {
        const int srcCh = juce::jmin (ch, channels - 1);
        auto* out = sd->buffer.getWritePointer (ch);
        const juce::uint8* p = r.d + dataOffset + (size_t) (srcCh * bytesPerSample);
        for (int i = 0; i < frames; ++i, p += frameBytes)
        {
            float v;
            if (bits == 16)
            {
                auto x = (juce::int16) (be ? ((p[0] << 8) | p[1]) : ((p[1] << 8) | p[0]));
                v = (float) x / 32768.0f;
            }
            else if (bits == 24)
            {
                int x = be ? ((p[0] << 24) | (p[1] << 16) | (p[2] << 8)) : ((p[2] << 24) | (p[1] << 16) | (p[0] << 8));
                v = (float) x / 2147483648.0f;
            }
            else
            {
                v = (float) (juce::int8) p[0] / 128.0f;
            }
            out[i] = v;
        }
    }

    if (applyPoints (*sd, frames, attributes))
        return sd;

    trimLeadingSilence (*sd);
    return sd;
}

void SampleLibrary::loadKorg (Instrument& inst)
{
    juce::MemoryBlock mb;
    if (! inst.source.loadFileAsData (mb))
    {
        inst.warnings.add (T ("Cannot read the KMP"));
        return;
    }

    ChunkReader r { (const juce::uint8*) mb.getData(), mb.getSize() };
    auto chunks = r.readChunks();
    auto* msp = ChunkReader::find (chunks, "MSP1");
    auto* rlp = ChunkReader::find (chunks, "RLP1");
    if (msp == nullptr || rlp == nullptr || msp->size < 18)
    {
        inst.warnings.add (T ("Not a Korg KMP file"));
        return;
    }

    const int numRegions = juce::jmin ((int) r.d[msp->offset + 16], (int) (rlp->size / 18));

    // KSF fajlovi su obicno u folderu istog imena kao KMP, ili pored njega
    const auto kmpDir = inst.source.getParentDirectory();
    const juce::File dirs[] = { kmpDir.getChildFile (inst.source.getFileNameWithoutExtension()), kmpDir };

    std::map<juce::String, int> loaded;   // ksf ime -> indeks sempla
    int prevTop = -1;

    for (int i = 0; i < numRegions; ++i)
    {
        const size_t p = rlp->offset + (size_t) i * 18;
        const juce::uint8 origKey = r.d[p];
        const int topKey = r.d[p + 1] & 0x7f;
        const int tune = (juce::int8) r.d[p + 2];
        const int level = (juce::int8) r.d[p + 3];
        const int pan = r.d[p + 4] & 0x7f;
        const auto fileName = r.text (p + 6, 12);

        const int lowKey = prevTop + 1;
        prevTop = juce::jmax (prevTop, topKey);

        if (fileName.isEmpty() || fileName.equalsIgnoreCase ("SKIPPEDSAMPL"))
            continue;
        if (fileName.startsWithIgnoreCase ("INTERNAL"))
        {
            inst.warnings.add (T ("Region ") + juce::String (i + 1) + T (" uses an internal ROM sample of the keyboard"));
            continue;
        }

        int sampleIdx = -1;
        if (auto it = loaded.find (fileName.toLowerCase()); it != loaded.end())
        {
            sampleIdx = it->second;
        }
        else
        {
            juce::File ksf;
            for (auto& d : dirs)
                if (! ksf.existsAsFile())
                    ksf = findChildIgnoringCase (d, fileName);

            if (! ksf.existsAsFile())
            {
                inst.warnings.add (T ("KSF file missing: ") + fileName);
                continue;
            }

            if (auto s = loadKsf (ksf, ksf.getRelativePathFrom (kmpDir).replaceCharacter ('\\', '/'), inst.warnings))
            {
                sampleIdx = (int) inst.samples.size();
                inst.samples.push_back (s);
            }
            loaded[fileName.toLowerCase()] = sampleIdx;
        }

        if (sampleIdx < 0)
            continue;

        Zone z;
        z.sample = sampleIdx;
        z.lowKey = juce::jlimit (0, 127, lowKey);
        z.highKey = juce::jlimit (z.lowKey, 127, topKey);
        z.rootKey = origKey & 0x7f;
        z.fixedPitch = (origKey & 0x80) != 0;
        z.tuneCents = (float) tune;
        z.gainDb = (float) level * 0.12f;
        z.pan = juce::jlimit (-1.0f, 1.0f, (float) (pan - 64) / 63.0f);
        inst.zones.push_back (z);
    }
}

//==============================================================================
//  Kontakt NKI
//==============================================================================
namespace
{
    // Kontakt 2/3 NKI: zaglavlje (~170 bajtova) + zlib-kompresovan XML
    juce::String extractNkiXml (const juce::MemoryBlock& mb)
    {
        auto* d = (const juce::uint8*) mb.getData();
        const size_t len = mb.getSize();

        juce::String raw (juce::CharPointer_UTF8 ((const char*) d), juce::jmin (len, (size_t) 4096));
        if (raw.containsIgnoreCase ("<?xml") || raw.containsIgnoreCase ("<K2_"))
            return juce::String::createStringFromData (d, (int) len);

        const size_t searchEnd = juce::jmin (len > 2 ? len - 2 : 0, (size_t) 65536);
        for (size_t i = 0; i < searchEnd; ++i)
        {
            if (d[i] != 0x78 || ! (d[i + 1] == 0x01 || d[i + 1] == 0x5e || d[i + 1] == 0x9c || d[i + 1] == 0xda))
                continue;

            juce::MemoryInputStream in (d + i, len - i, false);
            juce::GZIPDecompressorInputStream gz (&in, false, juce::GZIPDecompressorInputStream::zlibFormat);
            juce::MemoryBlock out;
            char buf[16384];
            while (out.getSize() < 64 * 1024 * 1024)
            {
                const int n = gz.read (buf, sizeof (buf));
                if (n <= 0) break;
                out.append (buf, (size_t) n);
            }
            if (out.getSize() < 32)
                continue;

            auto text = juce::String::createStringFromData (out.getData(), (int) out.getSize());
            if (text.contains ("<") && text.containsIgnoreCase ("zone"))
                return text;
        }
        return {};
    }

    struct NkiZone
    {
        juce::String file;
        int group = -1;
        int lowKey = 0, highKey = 127, lowVel = 1, highVel = 127, root = -1;
        float tune = 0.0f, gainDb = 0.0f, pan = 0.0f;
        int sampleStart = 0, loopStart = -1, loopEnd = -1;
    };

    struct NkiGroup { float gainDb = 0.0f; bool release = false; };

    bool isZoneTag (const juce::String& t)  { return t.equalsIgnoreCase ("Zone") || t.endsWithIgnoreCase ("_Zone"); }
    bool isGroupTag (const juce::String& t) { return t.equalsIgnoreCase ("Group") || t.endsWithIgnoreCase ("_Group"); }

    using ValueMap = std::map<juce::String, juce::String>;

    void collectValues (const juce::XmlElement& e, ValueMap& values, bool isRoot)
    {
        const auto& tag = e.getTagName();
        if (! isRoot && (isZoneTag (tag) || isGroupTag (tag) || tag.equalsIgnoreCase ("Loops")))
            return;

        for (int i = 0; i < e.getNumAttributes(); ++i)
            values.emplace (e.getAttributeName (i).toLowerCase(), e.getAttributeValue (i));

        if (e.hasAttribute ("name") && e.hasAttribute ("value"))
            values.emplace (e.getStringAttribute ("name").toLowerCase(), e.getStringAttribute ("value"));

        for (auto* c : e.getChildIterator())
            collectValues (*c, values, false);
    }

    bool isAudioName (const juce::String& s)
    {
        auto l = s.toLowerCase().trim();
        return l.endsWith (".wav") || l.endsWith (".aif") || l.endsWith (".aiff") || l.endsWith (".flac") || l.endsWith (".ncw");
    }

    bool has (const ValueMap& v, const char* k) { return v.find (k) != v.end(); }
    float getF (const ValueMap& v, const char* k, float def)
    {
        auto it = v.find (k);
        return it != v.end() ? it->second.getFloatValue() : def;
    }
    int getI (const ValueMap& v, std::initializer_list<const char*> keys, int def)
    {
        for (auto* k : keys)
            if (auto it = v.find (k); it != v.end() && it->second.trim().containsOnly ("-0123456789") && it->second.isNotEmpty())
                return it->second.getIntValue();
        return def;
    }

    void findGroups (const juce::XmlElement& e, std::map<int, NkiGroup>& groups, int& counter)
    {
        if (isGroupTag (e.getTagName()))
        {
            ValueMap v;
            collectValues (e, v, true);
            NkiGroup g;
            const float vol = getF (v, "volume", 1.0f);
            g.gainDb = vol > 0.0f ? juce::Decibels::gainToDecibels (vol) : 0.0f;
            g.release = v["releasetrigger"].equalsIgnoreCase ("yes");
            groups[e.getIntAttribute ("index", counter)] = g;
            ++counter;
            return;
        }
        for (auto* c : e.getChildIterator())
            findGroups (*c, groups, counter);
    }

    void findXmlZones (const juce::XmlElement& e, std::vector<NkiZone>& zones)
    {
        if (isZoneTag (e.getTagName()))
        {
            ValueMap v;
            collectValues (e, v, true);
            const bool k2 = e.getTagName().startsWithIgnoreCase ("K2_");

            NkiZone z;
            z.group = e.getIntAttribute ("groupIdx", -1);
            z.lowKey = getI (v, { "lowkey", "keylow", "lokey" }, 0);
            z.highKey = getI (v, { "highkey", "keyhigh", "hikey" }, 127);
            z.lowVel = getI (v, { "lowvelocity", "lowvel", "vellow", "lovel" }, 1);
            z.highVel = getI (v, { "highvelocity", "highvel", "velhigh", "hivel" }, 127);
            z.root = getI (v, { "rootkey", "root", "rootnote", "basenote" }, -1);
            z.sampleStart = juce::jmax (0, getI (v, { "samplestart" }, 0));

            if (k2 || has (v, "zonetune") || has (v, "zonevolume"))
            {
                // Kontakt 2: jacina je linearni faktor, stim je odnos frekvencija
                const float vol = getF (v, "zonevolume", 1.0f);
                const float tune = getF (v, "zonetune", 1.0f);
                z.gainDb = vol > 0.0f ? juce::Decibels::gainToDecibels (vol) : -60.0f;
                z.tune = tune > 0.0f ? 1200.0f * std::log2 (tune) : 0.0f;
                z.pan = getF (v, "zonepan", 0.0f);
            }
            else
            {
                z.tune = getF (v, "tune", 0.0f);
                z.gainDb = getF (v, "volume", 0.0f);
                z.pan = getF (v, "pan", 0.0f);
                if (std::abs (z.tune) < 49.0f) z.tune *= 100.0f;
            }
            if (std::abs (z.pan) > 1.0f) z.pan /= 100.0f;
            z.pan = juce::jlimit (-1.0f, 1.0f, z.pan);
            z.gainDb = juce::jlimit (-60.0f, 24.0f, z.gainDb);
            z.tune = juce::jlimit (-1200.0f, 1200.0f, z.tune);

            // loop zone
            if (auto* loops = e.getChildByName ("Loops"))
            {
                if (auto* loop = loops->getFirstChildElement())
                {
                    ValueMap lv;
                    collectValues (*loop, lv, true);
                    const auto mode = lv["mode"].toLowerCase();
                    const int ls = getI (lv, { "loopstart" }, -1);
                    const int ll = getI (lv, { "looplength" }, 0);
                    if (ls >= 0 && ll > 16 && mode != "no_loop" && mode != "off" && mode != "oneshot")
                    {
                        z.loopStart = ls;
                        z.loopEnd = ls + ll;
                    }
                }
            }

            for (auto* key : { "file_ex2", "file_ex", "file", "filename", "samplefile" })
                if (auto it = v.find (key); it != v.end() && it->second.isNotEmpty())
                {
                    z.file = it->second;
                    break;
                }
            if (z.file.isEmpty())
                for (auto& kv : v)
                    if (isAudioName (kv.second))
                        z.file = kv.second;

            if (z.file.isNotEmpty())
                zones.push_back (z);
            return;
        }

        for (auto* c : e.getChildIterator())
            findXmlZones (*c, zones);
    }

    // Novi NKI formati su binarni: izvuci nazive audio fajlova (ASCII i UTF-16)
    juce::StringArray findFileNamesInBinary (const juce::MemoryBlock& mb)
    {
        juce::StringArray found;
        auto* d = (const juce::uint8*) mb.getData();
        const size_t len = mb.getSize();

        auto consider = [&found] (const juce::String& s)
        {
            auto l = s.toLowerCase();
            for (auto* ext : { ".wav", ".aiff", ".aif", ".flac", ".ncw" })
            {
                const int idx = l.indexOf (ext);
                if (idx > 0)
                    found.addIfNotAlreadyThere (s.substring (0, idx + (int) strlen (ext)));
            }
        };

        juce::String cur;
        for (size_t i = 0; i < len; ++i)
        {
            const auto c = d[i];
            if (c >= 32 && c < 127) cur += (juce::juce_wchar) c;
            else { if (cur.length() >= 5) consider (cur); cur.clear(); }
        }
        if (cur.length() >= 5) consider (cur);

        for (size_t start = 0; start < 2; ++start)
        {
            cur.clear();
            for (size_t i = start; i + 1 < len; i += 2)
            {
                const auto c = d[i];
                if (d[i + 1] == 0 && c >= 32 && c < 127) cur += (juce::juce_wchar) c;
                else { if (cur.length() >= 5) consider (cur); cur.clear(); }
            }
            if (cur.length() >= 5) consider (cur);
        }
        return found;
    }

    juce::String lastPathPart (const juce::String& s)
    {
        return s.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false).trim();
    }

    // Kontakt putanja: "@d0184 Clarinet SamplesF-0001029000ime.wav"
    //   d + 3 cifre duzine + ime foldera, ... , F + 11 znakova + ime fajla
    void decodeKontaktPath (const juce::String& s, juce::StringArray& dirs, juce::String& fileName)
    {
        dirs.clear();
        fileName.clear();

        if (s.startsWithChar ('@'))
        {
            int i = 1;
            const int n = s.length();
            while (i < n)
            {
                const auto c = s[i];
                if (c == 'd' && i + 4 <= n && s.substring (i + 1, i + 4).containsOnly ("0123456789"))
                {
                    const int len = s.substring (i + 1, i + 4).getIntValue();
                    dirs.add (s.substring (i + 4, i + 4 + len));
                    i += 4 + len;
                }
                else if (c == 'b')
                {
                    dirs.add ("..");
                    i += 1;
                }
                else if (c == 'F' && i + 12 <= n)
                {
                    fileName = s.substring (i + 12);
                    break;
                }
                else
                {
                    ++i;
                }
            }
        }
        else
        {
            dirs.addTokens (s.replaceCharacter ('\\', '/'), "/", {});
            dirs.removeEmptyStrings();
            if (! dirs.isEmpty())
            {
                fileName = dirs[dirs.size() - 1];
                dirs.remove (dirs.size() - 1);
            }
        }

        if (fileName.isEmpty())
            fileName = lastPathPart (s);
    }
}

void SampleLibrary::loadKontakt (Instrument& inst, bool c3Is60)
{
    juce::MemoryBlock mb;
    if (! inst.source.loadFileAsData (mb))
    {
        inst.warnings.add (T ("Cannot read the NKI"));
        return;
    }

    const auto nkiDir = inst.source.getParentDirectory();
    const auto ownSamples = nkiDir.getChildFile (inst.source.getFileNameWithoutExtension() + " Samples");

    // indeks audio fajlova (pravi se samo ako putanja iz NKI-ja ne pogodi)
    std::map<juce::String, juce::File> index;
    bool indexBuilt = false;
    auto buildIndex = [&]
    {
        if (indexBuilt) return;
        indexBuilt = true;
        juce::Array<juce::File> dirs;
        for (auto d : { ownSamples, nkiDir, nkiDir.getParentDirectory().getChildFile ("Samples"),
                        nkiDir.getParentDirectory().getParentDirectory().getChildFile ("Samples") })
            if (d.isDirectory() && ! dirs.contains (d))
                dirs.add (d);
        int count = 0;
        for (auto& dir : dirs)
            for (const auto& e : juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFiles))
                if (isAudioFile (e.getFile()) && ++count < 50000)
                    index.emplace (e.getFile().getFileName().toLowerCase(), e.getFile());   // prvi (najblizi) pobedjuje
    };

    auto lookup = [&] (const juce::String& ref) -> juce::File
    {
        juce::StringArray dirs;
        juce::String name;
        decodeKontaktPath (ref, dirs, name);

        auto tryName = [] (const juce::File& dir, const juce::String& n) -> juce::File
        {
            auto f = findChildIgnoringCase (dir, n);
            if (! f.existsAsFile() && n.endsWithIgnoreCase (".ncw"))
                f = findChildIgnoringCase (dir, n.dropLastCharacters (4) + ".wav");
            return f;
        };

        // 1) tacna putanja relativno od NKI-ja
        auto dir = nkiDir;
        for (auto& d : dirs)
            dir = d == ".." ? dir.getParentDirectory() : dir.getChildFile (d);
        if (auto f = tryName (dir, name); f.existsAsFile())
            return f;

        // 2) "<ime> Samples" pored NKI-ja, pa sam folder NKI-ja
        for (auto& d : { ownSamples, nkiDir })
            if (auto f = tryName (d, name); f.existsAsFile())
                return f;

        // 3) pretraga po imenu
        buildIndex();
        auto lname = name.toLowerCase();
        for (int skip = 0; skip <= lname.length() - 5; ++skip)
        {
            auto n = lname.substring (skip);
            if (auto it = index.find (n); it != index.end())
                return it->second;
            if (n.endsWith (".ncw"))
                if (auto it2 = index.find (n.dropLastCharacters (4) + ".wav"); it2 != index.end())
                    return it2->second;
        }
        return {};
    };

    std::map<juce::String, int> loaded;  // putanja -> indeks sempla
    auto loadRef = [&] (const juce::File& f) -> int
    {
        auto key = f.getFullPathName();
        if (auto it = loaded.find (key); it != loaded.end())
            return it->second;
        int idx = -1;
        if (auto s = loadAudioFile (f, f.getRelativePathFrom (nkiDir).replaceCharacter ('\\', '/'), inst.warnings))
        {
            idx = (int) inst.samples.size();
            inst.samples.push_back (s);
        }
        loaded[key] = idx;
        return idx;
    };

    int missing = 0, ncw = 0, releaseSkipped = 0;

    // 1) Kontakt 1/2/3: XML sa zonama
    std::vector<NkiZone> xmlZones;
    std::map<int, NkiGroup> groups;
    auto xmlText = extractNkiXml (mb);
    if (xmlText.isNotEmpty())
    {
        auto start = xmlText.indexOfChar ('<');
        if (auto xml = juce::XmlDocument::parse (xmlText.substring (juce::jmax (0, start))))
        {
            int counter = 0;
            findGroups (*xml, groups, counter);
            findXmlZones (*xml, xmlZones);
        }
    }

    if (! xmlZones.empty())
    {
        for (auto& nz : xmlZones)
        {
            NkiGroup g;
            if (auto it = groups.find (nz.group); it != groups.end())
                g = it->second;
            if (g.release)
            {
                ++releaseSkipped;
                continue;
            }

            auto f = lookup (nz.file);
            if (! f.existsAsFile())
            {
                ++missing;
                if (nz.file.endsWithIgnoreCase (".ncw")) ++ncw;
                continue;
            }
            const int idx = loadRef (f);
            if (idx < 0) continue;

            const auto& smp = *inst.samples[(size_t) idx];
            Zone z;
            z.sample = idx;
            z.lowKey = juce::jlimit (0, 127, nz.lowKey);
            z.highKey = juce::jlimit (z.lowKey, 127, nz.highKey);
            z.lowVel = juce::jlimit (1, 127, nz.lowVel);
            z.highVel = juce::jlimit (z.lowVel, 127, nz.highVel);
            int rootKey = nz.root;
            if (rootKey < 0)
            {
                int n, layer;
                rootKey = parseFileName (smp.displayName, c3Is60, n, layer) ? n
                        : (smp.fileRootKey >= 0 ? smp.fileRootKey : 60);
            }
            z.rootKey = juce::jlimit (0, 127, rootKey);
            z.tuneCents = nz.tune;
            z.gainDb = juce::jlimit (-60.0f, 24.0f, nz.gainDb + g.gainDb);
            z.pan = nz.pan;
            const int len = smp.buffer.getNumSamples();
            if (nz.sampleStart > 0 && nz.sampleStart < len - 64)
                z.sampleStart = nz.sampleStart;
            if (nz.loopStart >= 0 && nz.loopEnd <= len && nz.loopEnd > nz.loopStart + 16)
            {
                z.loopStart = nz.loopStart;
                z.loopEnd = nz.loopEnd;
            }
            else if (nz.loopStart < 0 && smp.hasLoop)
            {
                z.loop = false;   // Kontakt zona bez loop-a: ne koristi loop iz WAV-a
            }
            inst.zones.push_back (z);
        }
    }
    else
    {
        // 2) noviji NKI (Kontakt 4+): nadji koje fajlove koristi, mapiraj po nazivima / smpl root
        auto refs = findFileNamesInBinary (mb);
        std::vector<int> indices;
        for (auto& ref : refs)
        {
            auto f = lookup (ref);
            if (! f.existsAsFile())
            {
                ++missing;
                if (ref.endsWithIgnoreCase (".ncw")) ++ncw;
                continue;
            }
            const int idx = loadRef (f);
            if (idx >= 0 && std::find (indices.begin(), indices.end(), idx) == indices.end())
                indices.push_back (idx);
        }

        if (indices.empty())
        {
            // ne vidimo nazive: uzmi semplove iz "<ime> Samples" foldera (ili pored NKI-ja)
            juce::Array<juce::File> files;
            const bool own = ownSamples.isDirectory();
            for (const auto& e : juce::RangedDirectoryIterator (own ? ownSamples : nkiDir, own, "*", juce::File::findFiles))
                if (isAudioFile (e.getFile()) && files.size() < 2000)
                    files.add (e.getFile());
            files.sort();
            for (auto& f : files)
            {
                const int idx = loadRef (f);
                if (idx >= 0) indices.push_back (idx);
            }
            missing = 0;
        }

        if (! indices.empty())
            inst.warnings.add (T ("NKI is a newer Kontakt format. The mapping was made from the samples (name / root note in the WAV). Check it in the editor."));
        else
            inst.warnings.add (T ("NKI is a newer, protected Kontakt format (samples packed in NKX/NCW). Its samples cannot be read: save it from Kontakt as Patch + samples, or resample it."));

        autoMapByName (inst, indices, c3Is60);
    }

    if (releaseSkipped > 0)
        inst.warnings.add (juce::String (releaseSkipped) + T (" release zones (play when the key is released) are skipped for now"));
    if (ncw > 0)
        inst.warnings.add (juce::String (ncw) + T (" samples are in Kontakt NCW format (compressed), not supported"));
    else if (missing > 0)
        inst.warnings.add (juce::String (missing) + T (" samples not found"));
}

//==============================================================================
//  Sacuvana mapa
//==============================================================================
bool SampleLibrary::applyMapFile (Instrument& inst)
{
    auto file = inst.getMapFile();
    if (! file.existsAsFile())
        return false;

    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("LiveSamplerMap"))
        return false;

    std::map<juce::String, int> byId;
    for (size_t i = 0; i < inst.samples.size(); ++i)
        byId[inst.samples[i]->id.toLowerCase()] = (int) i;

    std::vector<Zone> zones;
    std::set<int> used;
    for (auto* e : xml->getChildWithTagNameIterator ("Zone"))
    {
        auto it = byId.find (e->getStringAttribute ("sample").toLowerCase());
        if (it == byId.end())
            continue;

        Zone z;
        z.sample = it->second;
        z.enabled = e->getBoolAttribute ("enabled", true);
        z.lowKey = juce::jlimit (0, 127, e->getIntAttribute ("lowKey", 0));
        z.highKey = juce::jlimit (z.lowKey, 127, e->getIntAttribute ("highKey", 127));
        z.lowVel = juce::jlimit (1, 127, e->getIntAttribute ("lowVel", 1));
        z.highVel = juce::jlimit (z.lowVel, 127, e->getIntAttribute ("highVel", 127));
        z.rootKey = juce::jlimit (0, 127, e->getIntAttribute ("root", 60));
        z.tuneCents = (float) e->getDoubleAttribute ("tune", 0.0);
        z.gainDb = (float) e->getDoubleAttribute ("gain", 0.0);
        z.pan = (float) e->getDoubleAttribute ("pan", 0.0);
        z.loop = e->getBoolAttribute ("loop", true);
        z.fixedPitch = e->getBoolAttribute ("fixedPitch", false);
        z.sampleStart = juce::jmax (0, e->getIntAttribute ("sampleStart", 0));
        z.loopStart = e->getIntAttribute ("loopStart", -1);
        z.loopEnd = e->getIntAttribute ("loopEnd", -1);
        z.sampleEnd = e->getIntAttribute ("sampleEnd", -1);
        z.loopXfade = juce::jmax (0, e->getIntAttribute ("loopXfade", 0));
        zones.push_back (z);
        used.insert (z.sample);
    }

    // semplovi koji nisu u mapi ostaju vidljivi u editoru kao iskljucene zone
    for (int i = 0; i < (int) inst.samples.size(); ++i)
        if (used.count (i) == 0)
        {
            Zone z;
            z.sample = i;
            z.enabled = false;
            z.rootKey = inst.samples[(size_t) i]->fileRootKey >= 0 ? inst.samples[(size_t) i]->fileRootKey : 60;
            zones.push_back (z);
        }

    inst.zones = std::move (zones);
    inst.fromMapFile = true;
    return true;
}

//==============================================================================
InstrumentPtr SampleLibrary::loadInstrument (const juce::String& name, bool c3Is60, bool ignoreMapFile)
{
    Entry entry;
    {
        const juce::ScopedLock sl (lock);
        auto it = entries.find (name);
        if (it == entries.end())
            return nullptr;
        entry = it->second;
    }

    auto inst = std::make_shared<Instrument>();
    inst->name = name;
    inst->type = entry.type;
    inst->source = entry.file;

    switch (entry.type)
    {
        case SourceType::WavFolder: loadWavFolder (*inst, c3Is60); break;
        case SourceType::Korg:      loadKorg (*inst); break;
        case SourceType::Kontakt:   loadKontakt (*inst, c3Is60); break;
    }

    if (! ignoreMapFile)
        applyMapFile (*inst);

    return inst;
}

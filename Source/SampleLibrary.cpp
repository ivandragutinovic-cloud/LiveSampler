#include "SampleLibrary.h"
#include <regex>

//==============================================================================
const SampleData* Instrument::findSample (int note, int velocity) const
{
    for (auto& z : zones)
    {
        if (note >= z.lowKey && note <= z.highKey
            && velocity >= z.lowVel && velocity <= z.highVel
            && ! z.sampleIndices.empty())
        {
            auto n = (unsigned) z.sampleIndices.size();
            auto idx = n == 1 ? 0u : (roundRobinCounter.fetch_add (1) % n);
            return samples[(size_t) z.sampleIndices[idx]].get();
        }
    }
    return nullptr;
}

size_t Instrument::memoryBytes() const
{
    size_t total = 0;
    for (auto& s : samples)
        total += (size_t) s->buffer.getNumSamples() * (size_t) s->buffer.getNumChannels() * sizeof (float);
    return total;
}

//==============================================================================
SampleLibrary::SampleLibrary()
{
    formatManager.registerBasicFormats();
}

static bool isAudioFile (const juce::File& f)
{
    auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}

void SampleLibrary::scan (const juce::File& rootFolder)
{
    const juce::ScopedLock sl (lock);
    root = rootFolder;
    names.clear();
    folders.clear();

    if (! root.isDirectory())
        return;

    std::set<juce::File> dirs;
    for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
        if (isAudioFile (entry.getFile()))
            dirs.insert (entry.getFile().getParentDirectory());

    for (auto& d : dirs)
    {
        auto name = d == root ? root.getFileName()
                              : d.getRelativePathFrom (root).replaceCharacter ('\\', '/');
        folders[name] = d;
        names.add (name);
    }

    names.sortNatural();
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
InstrumentPtr SampleLibrary::loadInstrument (const juce::String& name, bool c3Is60)
{
    juce::File dir;
    {
        const juce::ScopedLock sl (lock);
        auto it = folders.find (name);
        if (it == folders.end())
            return nullptr;
        dir = it->second;
    }

    auto inst = std::make_shared<Instrument>();
    inst->name = name;

    juce::Array<juce::File> files;
    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
        if (isAudioFile (entry.getFile()))
            files.add (entry.getFile());

    struct Parsed { int sampleIndex; int note; int layer; };
    std::vector<Parsed> parsed;

    for (auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader;
        {
            const juce::ScopedLock sl (lock);
            reader.reset (formatManager.createReaderFor (f));
        }
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            inst->warnings.add ("Ne mogu da procitam: " + f.getFileName());
            continue;
        }

        auto sd = std::make_unique<SampleData>();
        sd->fileName = f.getFileName();
        sd->sampleRate = reader->sampleRate;

        const int len = (int) reader->lengthInSamples;
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
        int smplRoot = meta.containsKey ("MidiUnityNote") ? meta.getValue ("MidiUnityNote", "-1").getIntValue() : -1;

        // preskoci tisinu na pocetku (ispod oko -70 dB)
        {
            const float thr = 0.0003f;
            int first = 0;
            auto* l = sd->buffer.getReadPointer (0);
            auto* r = sd->buffer.getReadPointer (1);
            while (first < len && std::abs (l[first]) < thr && std::abs (r[first]) < thr)
                ++first;
            first = juce::jmax (0, first - 32);
            if (sd->hasLoop)
                first = juce::jmin (first, sd->loopStart);
            sd->startOffset = first < len - 64 ? first : 0;
        }

        int note = -1, layer = 0;
        if (! parseFileName (f.getFileNameWithoutExtension(), c3Is60, note, layer))
            note = (smplRoot >= 0 && smplRoot <= 127) ? smplRoot : -1;

        if (note < 0)
        {
            if (files.size() == 1)
                note = 60; // jedan fajl bez note: svira na celoj klavijaturi
            else
            {
                inst->warnings.add ("Nema note u nazivu: " + f.getFileName());
                continue;
            }
        }

        sd->rootNote = note;
        parsed.push_back ({ (int) inst->samples.size(), note, layer });
        inst->samples.push_back (std::move (sd));
    }

    if (parsed.empty())
        return inst;

    // velocity slojevi
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

        // root note u ovom sloju -> lista semplova (vise = round robin)
        std::map<int, std::vector<int>> byRoot;
        for (auto& p : parsed)
            if (p.layer == layers[(size_t) li])
                byRoot[p.note].push_back (p.sampleIndex);

        std::vector<int> roots;
        for (auto& kv : byRoot)
            roots.push_back (kv.first);

        for (size_t i = 0; i < roots.size(); ++i)
        {
            Zone z;
            z.lowKey  = i == 0 ? 0 : (roots[i - 1] + roots[i]) / 2 + 1;
            z.highKey = i == roots.size() - 1 ? 127 : (roots[i] + roots[i + 1]) / 2;
            z.lowVel = lowVel;
            z.highVel = highVel;
            z.sampleIndices = byRoot[roots[i]];
            inst->zones.push_back (z);
        }
    }

    return inst;
}

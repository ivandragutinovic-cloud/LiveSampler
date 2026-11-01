#include "Registers.h"

void Registers::loadFrom (const juce::File& file)
{
    for (auto& s : slots)
        s = Slot();

    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("Registers"))
        return;

    for (auto* e : xml->getChildWithTagNameIterator ("Slot"))
    {
        const int idx = e->getIntAttribute ("index", -1);
        if (idx < 0 || idx >= numSlots)
            continue;

        auto& s = slots[(size_t) idx];
        s.used = true;
        for (int p = 0; p < numParts; ++p)
            s.instruments[p] = e->getStringAttribute ("inst" + juce::String (p));

        for (auto* v : e->getChildWithTagNameIterator ("Param"))
            s.values.set (v->getStringAttribute ("id"), v->getDoubleAttribute ("value"));
    }
}

void Registers::saveTo (const juce::File& file) const
{
    juce::XmlElement root ("Registers");
    for (int i = 0; i < numSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (! s.used)
            continue;

        auto* e = root.createNewChildElement ("Slot");
        e->setAttribute ("index", i);
        for (int p = 0; p < numParts; ++p)
            e->setAttribute ("inst" + juce::String (p), s.instruments[p]);

        for (auto& nv : s.values)
        {
            auto* v = e->createNewChildElement ("Param");
            v->setAttribute ("id", nv.name.toString());
            v->setAttribute ("value", (double) nv.value);
        }
    }

    file.getParentDirectory().createDirectory();
    root.writeTo (file);
}

juce::StringArray Registers::getAllInstrumentNames() const
{
    juce::StringArray names;
    for (auto& s : slots)
        if (s.used)
            for (auto& n : s.instruments)
                if (n.isNotEmpty())
                    names.addIfNotAlreadyThere (n);
    return names;
}

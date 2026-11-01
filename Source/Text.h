#pragma once
#include <JuceHeader.h>

// Tekst sa nasim slovima (č, ć, š, ž, đ). Izvorni fajlovi su UTF-8.
inline juce::String T (const char* utf8) { return juce::String::fromUTF8 (utf8); }

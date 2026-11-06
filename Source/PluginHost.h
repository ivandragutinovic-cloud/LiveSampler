#pragma once
#include <JuceHeader.h>
#include "Model.h"

// VST3 plugin u partu (npr. Audio Modeling SWAM): jedna instanca po useru, ostaje ucitana dok se menjaju
// performansi (bez ponovnog ucitavanja na svirci). Audio nit samo puni MIDI i zove processBlock.
struct HostedPlugin
{
    std::unique_ptr<juce::AudioPluginInstance> inst;
    juce::String userName, error;
    juce::MidiBuffer midi;                 // audio nit: dogadjaji za sledeci blok
    juce::AudioBuffer<float> buf;          // audio nit
    std::array<juce::int8, 128> played {};  // koji ton plugin svira za pritisnut taster (-1 = nijedan)
    int tailSamples = 0;                   // audio nit: koliko jos da svira posle promene performansa
    HostedPlugin() { played.fill (-1); }
};

class PluginHost
{
public:
    PluginHost();
    ~PluginHost();

    // pretraga VST3 foldera (pozadina); done se zove na message niti
    void startScan (std::function<void()> done);
    bool isScanning() const;
    juce::String scanStatus() const;
    juce::Array<juce::PluginDescription> instruments() const;
    int numKnown() const { return known.getNumTypes(); }

    // message nit: instanca za usera (pravi je jednom, posle se samo vraca); nullptr ako ne moze
    std::shared_ptr<HostedPlugin> instanceFor (const UserProgram& u, double sampleRate, int blockSize);
    std::shared_ptr<HostedPlugin> find (const juce::String& userName) const;
    void prepareAll (double sampleRate, int blockSize);     // audio stoji
    bool captureState (const juce::String& userName, juce::String& stateOut);   // stanje plugina u base64
    void forget (const juce::String& userName);              // user obrisan / promenjen plugin
    void rename (const juce::String& from, const juce::String& to);
    void collectGarbage();                                   // message nit, periodicno

    static juce::String describe (const juce::String& pluginDescXml);

private:
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    std::map<juce::String, std::shared_ptr<HostedPlugin>> cache;
    std::vector<std::shared_ptr<HostedPlugin>> graveyard;
    juce::File listFile() const;
    class ScanThread;
    std::unique_ptr<ScanThread> scanThread;
    juce::String lastScanStatus;
    double sr = 44100.0;
    int block = 512;
};

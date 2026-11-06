#include "PluginHost.h"

static juce::File dataDir() { return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LiveSampler"); }

class PluginHost::ScanThread : public juce::Thread
{
public:
    ScanThread (PluginHost& h, std::function<void()> d) : juce::Thread ("VST3 scan"), host (h), done (std::move (d)) {}
    void run() override
    {
        for (int f = 0; f < host.formats.getNumFormats(); ++f)
        {
            auto* format = host.formats.getFormat (f);
            // plugin koji obori pretragu se pamti u "dead man's pedal" fajlu i sledeci put preskace
            juce::PluginDirectoryScanner scanner (host.known, *format, format->getDefaultLocationsToSearch(), true,
                                                  dataDir().getChildFile ("vst-scan-crash.txt"), true);
            juce::String name;
            while (! threadShouldExit())
            {
                const auto next = scanner.getNextPluginFileThatWillBeScanned();
                juce::MessageManager::callAsync ([&h = host, next] { h.lastScanStatus = "Scanning: " + juce::File (next).getFileName(); });
                if (! scanner.scanNextFile (true, name)) break;
            }
        }
        if (auto xml = host.known.createXml())
        {
            dataDir().createDirectory();
            xml->writeTo (host.listFile());
        }
        juce::MessageManager::callAsync ([&h = host, d = done]
        {
            h.lastScanStatus = juce::String (h.instruments().size()) + " VST3 instruments found";
            if (d) d();
        });
    }
    PluginHost& host;
    std::function<void()> done;
};

PluginHost::PluginHost()
{
   #if JUCE_PLUGINHOST_VST3
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
   #endif
    if (auto xml = juce::parseXML (listFile()))
        known.recreateFromXml (*xml);
}

PluginHost::~PluginHost()
{
    if (scanThread != nullptr) scanThread->stopThread (5000);
}

bool PluginHost::isScanning() const { return scanThread != nullptr && scanThread->isThreadRunning(); }

juce::File PluginHost::listFile() const { return dataDir().getChildFile ("vst-plugins.xml"); }

void PluginHost::startScan (std::function<void()> done)
{
    if (isScanning()) return;
    lastScanStatus = "Scanning VST3 folder...";
    scanThread = std::make_unique<ScanThread> (*this, std::move (done));
    scanThread->startThread();
}

juce::String PluginHost::scanStatus() const
{
    if (lastScanStatus.isNotEmpty()) return lastScanStatus;
    return juce::String (instruments().size()) + " VST3 instruments known";
}

juce::Array<juce::PluginDescription> PluginHost::instruments() const
{
    juce::Array<juce::PluginDescription> out;
    for (auto& d : known.getTypes())
        if (d.isInstrument) out.add (d);
    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.name.compareNatural (b.name) < 0; });
    return out;
}

juce::String PluginHost::describe (const juce::String& pluginDescXml)
{
    juce::PluginDescription d;
    if (auto x = juce::parseXML (pluginDescXml)) d.loadFromXml (*x);
    return d.name.isNotEmpty() ? d.name + (d.manufacturerName.isNotEmpty() ? " (" + d.manufacturerName + ")" : juce::String()) : juce::String ("?");
}

std::shared_ptr<HostedPlugin> PluginHost::find (const juce::String& userName) const
{
    auto it = cache.find (userName);
    return it != cache.end() ? it->second : nullptr;
}

std::shared_ptr<HostedPlugin> PluginHost::instanceFor (const UserProgram& u, double sampleRate, int blockSize)
{
    if (! u.isPlugin()) return nullptr;
    if (sampleRate > 0) sr = sampleRate;
    if (blockSize > 0) block = blockSize;
    if (auto it = cache.find (u.name); it != cache.end())
        return it->second->inst != nullptr ? it->second : nullptr;

    auto hp = std::make_shared<HostedPlugin>();
    hp->userName = u.name;
    juce::PluginDescription d;
    if (auto x = juce::parseXML (u.pluginDesc)) d.loadFromXml (*x);
    juce::String err;
    hp->inst = formats.createPluginInstance (d, sr, block, err);
    hp->error = err;
    if (hp->inst != nullptr)
    {
        hp->inst->enableAllBuses();
        hp->inst->setRateAndBufferSizeDetails (sr, block);
        if (u.pluginState.isNotEmpty())
        {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (u.pluginState)) hp->inst->setStateInformation (mb.getData(), (int) mb.getSize());
        }
        hp->inst->prepareToPlay (sr, block);
        hp->buf.setSize (juce::jmax (2, hp->inst->getTotalNumInputChannels(), hp->inst->getTotalNumOutputChannels()), block);
        hp->midi.ensureSize (2048);
    }
    cache[u.name] = hp;   // i neuspeh se pamti (da se ne pokusava na svaku izmenu)
    return hp->inst != nullptr ? hp : nullptr;
}

void PluginHost::prepareAll (double sampleRate, int blockSize)
{
    sr = sampleRate;
    block = blockSize;
    for (auto& [n, hp] : cache)
        if (hp->inst != nullptr)
        {
            hp->inst->releaseResources();
            hp->inst->setRateAndBufferSizeDetails (sr, block);
            hp->inst->prepareToPlay (sr, block);
            hp->buf.setSize (juce::jmax (2, hp->inst->getTotalNumInputChannels(), hp->inst->getTotalNumOutputChannels()), block);
        }
}

bool PluginHost::captureState (const juce::String& userName, juce::String& stateOut)
{
    auto hp = find (userName);
    if (hp == nullptr || hp->inst == nullptr) return false;
    juce::MemoryBlock mb;
    hp->inst->getStateInformation (mb);
    stateOut = mb.toBase64Encoding();
    return true;
}

void PluginHost::forget (const juce::String& userName)
{
    if (auto it = cache.find (userName); it != cache.end())
    {
        graveyard.push_back (it->second);   // audio nit mozda jos drzi config sa njim
        cache.erase (it);
    }
}

void PluginHost::rename (const juce::String& from, const juce::String& to)
{
    if (from == to) return;
    if (auto it = cache.find (from); it != cache.end())
    {
        auto hp = it->second;
        cache.erase (it);
        hp->userName = to;
        cache[to] = hp;
    }
}

void PluginHost::collectGarbage()
{
    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(), [] (const auto& p) { return p.use_count() <= 1; }), graveyard.end());
}

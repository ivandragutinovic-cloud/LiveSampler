#pragma once
#include <JuceHeader.h>

class LiveSamplerProcessor;

//==============================================================================
//  Remote control for Live Kontroler (iPad): a small web server inside Pikaso Workstation.
//    GET  /            the remote page (PIKASO tab in Live Kontroler shows it)
//    GET  /events      live state (Server-Sent Events): performance, users, the edited user, status
//    GET  /instruments list of multisamples (for the search)
//    POST /api         changes from the iPad (JSON)
//  Default port 8091. Works without the editor window (state comes from the processor).
//==============================================================================
class RemoteServer : private juce::Thread,
                     private juce::Timer,
                     private juce::ChangeListener
{
public:
    explicit RemoteServer (LiveSamplerProcessor&, int port = 8091);
    ~RemoteServer() override;

    int getPort() const { return port; }
    bool isListening() const { return listening.load(); }
    int getNumClients() const;

private:
    struct Client
    {
        juce::StreamingSocket* socket = nullptr;
        juce::CriticalSection lock;
        juce::StringArray queue;
        juce::WaitableEvent ready;
        bool needFull = true;
        bool dead = false;
    };

    void run() override;                                  // accept loop
    void handleConnection (std::unique_ptr<juce::StreamingSocket> s);
    bool readRequest (juce::StreamingSocket& s, juce::String& method, juce::String& path,
                      juce::StringPairArray& headers, juce::MemoryBlock& body);
    static bool writeAll (juce::StreamingSocket& s, const void* data, int size);
    static bool sendResponse (juce::StreamingSocket& s, int code, const juce::String& type, const juce::MemoryBlock& body, bool keepAlive);
    void serveEvents (juce::StreamingSocket& s);
    juce::String handleApi (const juce::String& json);

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { dirty = true; }
    juce::StringPairArray buildSections();
    void applyOnMessageThread (const juce::var& v);

    LiveSamplerProcessor& proc;
    const int port;
    juce::StreamingSocket listener;
    std::atomic<bool> listening { false }, stopping { false };
    std::atomic<int> active { 0 };

    juce::CriticalSection clientsLock;
    std::vector<std::shared_ptr<Client>> clients;
    juce::CriticalSection socketsLock;
    std::vector<juce::StreamingSocket*> openSockets;

    // poslednje poslato stanje po delu (salje se samo ono sto se promenilo)
    juce::StringPairArray lastSent;
    bool dirty = true;
    int remoteUser = -1;          // user koji se edituje na iPad-u (-1 = user dela 1)
    juce::String mapInst;
    bool mapDirty = false;        // zone promenjene, mapa jos nije sacuvana         // multisempl otvoren u MAPPING na iPad-u
    int mapUser = -1;             // user iz kog je otvoren (za listu njegovih multisemplova)
    void deleteMapZones (const juce::var& v);
    juce::String instrumentsJson { "[]" }, instrumentsVer;   // pod clientsLock
    std::shared_ptr<bool> alive { std::make_shared<bool> (true) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteServer)
};

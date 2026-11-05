#include "RemoteServer.h"
#include "PluginProcessor.h"
#include "SamplerPart.h"
#include "BinaryData.h"
#include <thread>
#include <set>
#if ! JUCE_WINDOWS
 #include <csignal>
#endif

namespace
{
    // XML (kao u podacima) -> JSON: atributi su polja, deca su nizovi po imenu taga
    juce::var xmlToVar (const juce::XmlElement& x)
    {
        static const juce::StringArray textKeys { "fmPatch", "name", "user", "instrument", "category", "categoryName", "kronosSource", "kronosPcg", "missingName", "missingSource", "missingPcg" };
        auto* o = new juce::DynamicObject();
        for (int i = 0; i < x.getNumAttributes(); ++i)
        {
            const auto k = x.getAttributeName (i);
            const auto v = x.getAttributeValue (i);
            const bool numeric = ! textKeys.contains (k) && v.isNotEmpty() && v.containsOnly ("0123456789.-+eE")
                                 && (v[0] == '-' || v[0] == '+' || v[0] == '.' || juce::CharacterFunctions::isDigit (v[0]));
            if (numeric) o->setProperty (k, v.getDoubleValue());
            else o->setProperty (k, v);
        }
        for (auto* c : x.getChildIterator())
        {
            auto arr = o->getProperty (c->getTagName());
            if (! arr.isArray()) { o->setProperty (c->getTagName(), juce::Array<juce::var>()); arr = o->getProperty (c->getTagName()); }
            arr.getArray()->add (xmlToVar (*c));
        }
        return juce::var (o);
    }

    // "Block[1]/AmpEG@aTime", "Part[3]@volume", "Master@revSize", "@voiceMode"
    bool setPath (juce::XmlElement& root, const juce::String& path, const juce::var& value)
    {
        auto elemPath = path.upToFirstOccurrenceOf ("@", false, false);
        const auto attr = path.fromFirstOccurrenceOf ("@", false, false);
        if (attr.isEmpty() || ! attr.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")) return false;
        juce::XmlElement* e = &root;
        for (auto seg : juce::StringArray::fromTokens (elemPath, "/", ""))
        {
            seg = seg.trim();
            if (seg.isEmpty()) continue;
            const auto tag = seg.upToFirstOccurrenceOf ("[", false, false);
            if (tag.isEmpty() || ! tag.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")) return false;
            juce::XmlElement* next = nullptr;
            if (seg.contains ("["))
            {
                const int idx = seg.fromFirstOccurrenceOf ("[", false, false).upToFirstOccurrenceOf ("]", false, false).getIntValue();
                for (auto* c : e->getChildWithTagNameIterator (tag))
                    if (c->getIntAttribute ("index", -1) == idx) { next = c; break; }
                if (next == nullptr)
                {
                    next = e->createNewChildElement (tag);
                    next->setAttribute ("index", idx);
                    if (tag == "Osc") next->setAttribute ("block", idx / kMsPerOsc);
                }
            }
            else
            {
                next = e->getChildByName (tag);
                if (next == nullptr) next = e->createNewChildElement (tag);
            }
            e = next;
        }
        if (value.isBool()) e->setAttribute (attr, (bool) value);
        else if (value.isInt() || value.isInt64()) e->setAttribute (attr, (int) value);
        else if (value.isDouble()) e->setAttribute (attr, (double) value);
        else e->setAttribute (attr, value.toString());
        return true;
    }

    juce::String json (const juce::var& v) { return juce::JSON::toString (v, true); }
}

//==============================================================================
RemoteServer::RemoteServer (LiveSamplerProcessor& p, int portToUse)
    : juce::Thread ("Pikaso remote"), proc (p), port (portToUse)
{
   #if ! JUCE_WINDOWS
    std::signal (SIGPIPE, SIG_IGN);     // zatvoren iPad ne sme da ugasi program
   #endif
    proc.addChangeListener (this);
    startThread();
    startTimerHz (15);
}

RemoteServer::~RemoteServer()
{
    stopTimer();
    proc.removeChangeListener (this);
    stopping = true;
    signalThreadShouldExit();
    listener.close();
    {
        const juce::ScopedLock sl (socketsLock);
        for (auto* s : openSockets) s->close();
    }
    {
        const juce::ScopedLock sl (clientsLock);
        for (auto& c : clients) c->ready.signal();
    }
    stopThread (3000);
    for (int i = 0; i < 60 && active.load() > 0; ++i) juce::Thread::sleep (50);
}

int RemoteServer::getNumClients() const
{
    const juce::ScopedLock sl (clientsLock);
    return (int) clients.size();
}

void RemoteServer::run()
{
    while (! threadShouldExit())
    {
        if (! listening.load())
        {
            if (listener.createListener (port, {})) listening = true;
            else { wait (3000); continue; }
        }
        std::unique_ptr<juce::StreamingSocket> s (listener.waitForNextConnection());
        if (s == nullptr)
        {
            if (threadShouldExit()) break;
            listening = false;
            listener.close();
            wait (500);
            continue;
        }
        ++active;
        std::thread ([this, raw = s.release()] { handleConnection (std::unique_ptr<juce::StreamingSocket> (raw)); --active; }).detach();
    }
}

bool RemoteServer::writeAll (juce::StreamingSocket& s, const void* data, int size)
{
    auto* p = static_cast<const char*> (data);
    while (size > 0)
    {
        const int n = s.write (p, size);
        if (n <= 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

bool RemoteServer::sendResponse (juce::StreamingSocket& s, int code, const juce::String& type, const juce::MemoryBlock& body, bool keepAlive)
{
    const char* text = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 400 ? "Bad Request" : "Error";
    juce::String h;
    h << "HTTP/1.1 " << code << " " << text << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << (int) body.getSize() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Access-Control-Allow-Headers: Content-Type\r\n"
      << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n\r\n";
    const auto hb = h.toStdString();
    return writeAll (s, hb.data(), (int) hb.size()) && (body.getSize() == 0 || writeAll (s, body.getData(), (int) body.getSize()));
}

bool RemoteServer::readRequest (juce::StreamingSocket& s, juce::String& method, juce::String& path,
                                juce::StringPairArray& headers, juce::MemoryBlock& body)
{
    std::string buf;
    char tmp[4096];
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos)
    {
        if (stopping.load()) return false;
        const int r = s.waitUntilReady (true, 20000);
        if (r <= 0) return false;
        const int n = s.read (tmp, (int) sizeof (tmp), false);
        if (n <= 0) return false;
        buf.append (tmp, (size_t) n);
        headEnd = buf.find ("\r\n\r\n");
        if (buf.size() > 65536) return false;
    }
    const auto head = juce::String::fromUTF8 (buf.data(), (int) headEnd);
    auto lines = juce::StringArray::fromLines (head);
    if (lines.isEmpty()) return false;
    auto first = juce::StringArray::fromTokens (lines[0], " ", "");
    if (first.size() < 2) return false;
    method = first[0].toUpperCase();
    path = first[1].upToFirstOccurrenceOf ("?", false, false);
    for (int i = 1; i < lines.size(); ++i)
        if (lines[i].contains (":"))
            headers.set (lines[i].upToFirstOccurrenceOf (":", false, false).trim().toLowerCase(), lines[i].fromFirstOccurrenceOf (":", false, false).trim());
    const int len = juce::jlimit (0, 4 * 1024 * 1024, headers["content-length"].getIntValue());
    std::string rest = buf.substr (headEnd + 4);
    while ((int) rest.size() < len)
    {
        if (s.waitUntilReady (true, 10000) <= 0) return false;
        const int n = s.read (tmp, (int) juce::jmin ((size_t) sizeof (tmp), (size_t) len - rest.size()), false);
        if (n <= 0) return false;
        rest.append (tmp, (size_t) n);
    }
    body.replaceAll (rest.data(), (size_t) len);
    return true;
}

void RemoteServer::handleConnection (std::unique_ptr<juce::StreamingSocket> s)
{
    {
        const juce::ScopedLock sl (socketsLock);
        openSockets.push_back (s.get());
    }
    while (! stopping.load())
    {
        juce::String method, path;
        juce::StringPairArray headers;
        juce::MemoryBlock body;
        if (! readRequest (*s, method, path, headers, body)) break;
        const bool keep = ! headers["connection"].equalsIgnoreCase ("close");

        if (method == "OPTIONS") { if (! sendResponse (*s, 200, "text/plain", {}, keep)) break; continue; }
        if (method == "GET" && (path == "/" || path == "/index.html"))
        {
            juce::MemoryBlock page (BinaryData::remote_html, (size_t) BinaryData::remote_htmlSize);
            if (! sendResponse (*s, 200, "text/html; charset=utf-8", page, keep)) break;
            continue;
        }
        if (method == "GET" && path == "/ws" && headers["upgrade"].equalsIgnoreCase ("websocket") && headers["sec-websocket-key"].isNotEmpty())
        {
            serveWebSocket (*s, headers["sec-websocket-key"]);
            break;
        }
        if (method == "GET" && path == "/events")
        {
            serveEvents (*s);
            break;
        }
        if (method == "GET" && path == "/instruments")
        {
            juce::String j;
            {
                const juce::ScopedLock sl (clientsLock);
                j = instrumentsJson;
            }
            const auto u8 = j.toStdString();
            if (! sendResponse (*s, 200, "application/json; charset=utf-8", juce::MemoryBlock (u8.data(), u8.size()), keep)) break;
            continue;
        }
        if (method == "POST" && path == "/api")
        {
            const auto reply = handleApi (juce::String::fromUTF8 ((const char*) body.getData(), (int) body.getSize())).toStdString();
            if (! sendResponse (*s, 200, "application/json; charset=utf-8", juce::MemoryBlock (reply.data(), reply.size()), keep)) break;
            continue;
        }
        const std::string nf = "not found";
        if (! sendResponse (*s, 404, "text/plain", juce::MemoryBlock (nf.data(), nf.size()), keep)) break;
    }
    {
        const juce::ScopedLock sl (socketsLock);
        openSockets.erase (std::remove (openSockets.begin(), openSockets.end(), s.get()), openSockets.end());
    }
    s->close();
}

void RemoteServer::serveEvents (juce::StreamingSocket& s)
{
    const std::string h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n: ok\n\n";
    if (! writeAll (s, h.data(), (int) h.size())) return;
    auto c = std::make_shared<Client>();
    c->socket = &s;
    {
        const juce::ScopedLock sl (clientsLock);
        clients.push_back (c);
    }
    auto lastWrite = juce::Time::getMillisecondCounter();
    while (! stopping.load())
    {
        c->ready.wait (250);
        juce::StringArray q;
        {
            const juce::ScopedLock sl (c->lock);
            q.swapWith (c->queue);
        }
        bool ok = true;
        for (auto& m : q)
        {
            const auto b = ("data: " + m + "\n\n").toStdString();
            if (! writeAll (s, b.data(), (int) b.size())) { ok = false; break; }
            lastWrite = juce::Time::getMillisecondCounter();
        }
        if (ok && juce::Time::getMillisecondCounter() - lastWrite > 10000)
        {
            ok = writeAll (s, ": ka\n\n", 6);
            lastWrite = juce::Time::getMillisecondCounter();
        }
        if (! ok) break;
    }
    const juce::ScopedLock sl (clientsLock);
    clients.erase (std::remove (clients.begin(), clients.end(), c), clients.end());
}


//==============================================================================
//  WebSocket (RFC 6455): handshake treba SHA-1 + base64
namespace
{
    std::array<juce::uint8, 20> sha1 (const std::string& msg)
    {
        juce::uint32 h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
        std::vector<juce::uint8> d (msg.begin(), msg.end());
        const juce::uint64 bits = (juce::uint64) d.size() * 8;
        d.push_back (0x80);
        while (d.size() % 64 != 56) d.push_back (0);
        for (int i = 7; i >= 0; --i) d.push_back ((juce::uint8) (bits >> (i * 8)));
        auto rol = [] (juce::uint32 x, int n) { return (x << n) | (x >> (32 - n)); };
        for (size_t c = 0; c < d.size(); c += 64)
        {
            juce::uint32 w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (juce::uint32) d[c + (size_t) i * 4] << 24 | (juce::uint32) d[c + (size_t) i * 4 + 1] << 16 | (juce::uint32) d[c + (size_t) i * 4 + 2] << 8 | d[c + (size_t) i * 4 + 3];
            for (int i = 16; i < 80; ++i) w[i] = rol (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            juce::uint32 a = h0, b = h1, cc = h2, dd = h3, e = h4;
            for (int i = 0; i < 80; ++i)
            {
                juce::uint32 f, k;
                if (i < 20)      { f = (b & cc) | (~b & dd); k = 0x5A827999; }
                else if (i < 40) { f = b ^ cc ^ dd; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & cc) | (b & dd) | (cc & dd); k = 0x8F1BBCDC; }
                else             { f = b ^ cc ^ dd; k = 0xCA62C1D6; }
                const juce::uint32 t = rol (a, 5) + f + e + k + w[i];
                e = dd; dd = cc; cc = rol (b, 30); b = a; a = t;
            }
            h0 += a; h1 += b; h2 += cc; h3 += dd; h4 += e;
        }
        std::array<juce::uint8, 20> out {};
        const juce::uint32 hs[] = { h0, h1, h2, h3, h4 };
        for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) out[(size_t) (i * 4 + j)] = (juce::uint8) (hs[i] >> (24 - j * 8));
        return out;
    }

    std::string wsFrame (const std::string& payload, int opcode = 1)
    {
        std::string f;
        f.push_back ((char) (0x80 | opcode));
        const auto n = payload.size();
        if (n < 126) f.push_back ((char) n);
        else if (n < 65536) { f.push_back ((char) 126); f.push_back ((char) (n >> 8)); f.push_back ((char) (n & 0xff)); }
        else { f.push_back ((char) 127); for (int i = 7; i >= 0; --i) f.push_back ((char) ((juce::uint64) n >> (i * 8))); }
        return f + payload;
    }
}

juce::String RemoteServer::webSocketAccept (const juce::String& key)
{
    const auto h = sha1 ((key.trim() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toStdString());
    return juce::Base64::toBase64 (h.data(), h.size());
}

void RemoteServer::serveWebSocket (juce::StreamingSocket& s, const juce::String& key)
{
    const std::string h = ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                           + webSocketAccept (key) + "\r\n\r\n").toStdString();
    if (! writeAll (s, h.data(), (int) h.size())) return;
    auto c = std::make_shared<Client>();
    c->socket = &s;
    {
        const juce::ScopedLock sl (clientsLock);
        clients.push_back (c);
    }
    std::string in;            // primljeni bajtovi koji jos nisu ceo frame
    std::string message;       // tekst iz vise delova (fragmentacija)
    auto lastWrite = juce::Time::getMillisecondCounter();
    bool ok = true;
    while (ok && ! stopping.load())
    {
        // 1) poslati stanje (isti red kao za /events)
        juce::StringArray q;
        {
            const juce::ScopedLock sl (c->lock);
            q.swapWith (c->queue);
        }
        for (auto& m : q)
        {
            const auto f = wsFrame (m.toStdString());
            if (! writeAll (s, f.data(), (int) f.size())) { ok = false; break; }
            lastWrite = juce::Time::getMillisecondCounter();
        }
        if (! ok) break;
        if (juce::Time::getMillisecondCounter() - lastWrite > 10000)
        {
            const auto f = wsFrame ({}, 9);   // ping: veza ostaje otvorena (WiFi, ruter)
            if (! writeAll (s, f.data(), (int) f.size())) break;
            lastWrite = juce::Time::getMillisecondCounter();
        }

        // 2) primiti komande sa iPad-a (ceka najvise 15 ms da bi slanje ostalo brzo)
        const int r = s.waitUntilReady (true, 15);
        if (r < 0) break;
        if (r == 0) continue;
        char tmp[8192];
        const int n = s.read (tmp, (int) sizeof (tmp), false);
        if (n <= 0) break;
        in.append (tmp, (size_t) n);
        if (in.size() > 8 * 1024 * 1024) break;
        while (in.size() >= 2)
        {
            const auto b0 = (juce::uint8) in[0], b1 = (juce::uint8) in[1];
            const int opcode = b0 & 0x0f;
            const bool fin = (b0 & 0x80) != 0, masked = (b1 & 0x80) != 0;
            size_t len = b1 & 0x7f, pos = 2;
            if (len == 126) { if (in.size() < 4) break; len = (size_t) ((juce::uint8) in[2] << 8 | (juce::uint8) in[3]); pos = 4; }
            else if (len == 127)
            {
                if (in.size() < 10) break;
                len = 0; for (int i = 0; i < 8; ++i) len = len << 8 | (juce::uint8) in[(size_t) (2 + i)];
                pos = 10;
            }
            if (len > 4 * 1024 * 1024) { ok = false; break; }
            const size_t maskPos = pos;
            if (masked) pos += 4;
            if (in.size() < pos + len) break;
            std::string payload = in.substr (pos, len);
            if (masked) for (size_t i = 0; i < len; ++i) payload[i] = (char) (payload[i] ^ in[maskPos + (i & 3)]);
            in.erase (0, pos + len);
            if (opcode == 8) { const auto f = wsFrame ({}, 8); writeAll (s, f.data(), (int) f.size()); ok = false; break; }
            if (opcode == 9) { const auto f = wsFrame (payload, 10); if (! writeAll (s, f.data(), (int) f.size())) { ok = false; break; } continue; }
            if (opcode == 10) continue;
            if (opcode == 1 || opcode == 0)
            {
                message += payload;
                if (fin) { handleApi (juce::String::fromUTF8 (message.data(), (int) message.size())); message.clear(); }
            }
        }
    }
    const juce::ScopedLock sl (clientsLock);
    clients.erase (std::remove (clients.begin(), clients.end(), c), clients.end());
}

juce::String RemoteServer::handleApi (const juce::String& text)
{
    auto v = juce::JSON::parse (text);
    if (! v.isObject() && ! v.isArray()) return "{\"ok\":false}";
    std::weak_ptr<bool> weak = alive;
    juce::MessageManager::callAsync ([this, weak, v]
    {
        if (weak.expired()) return;
        if (v.isArray()) { for (auto& x : *v.getArray()) applyOnMessageThread (x); }
        else applyOnMessageThread (v);
    });
    return "{\"ok\":true}";
}

void RemoteServer::applyOnMessageThread (const juce::var& v)
{
    const auto op = v["op"].toString();
    if (op == "set")
    {
        const auto target = v["target"].toString();
        const auto* sets = v["sets"].getArray();
        if (sets == nullptr) return;
        if (target == "perf")
        {
            auto xml = proc.getPerformance().toXml();
            bool any = false;
            for (auto& s : *sets)
                if (s.isArray() && s.size() >= 2) any = setPath (*xml, s[0].toString(), s[1]) || any;
            if (any) proc.updatePerformance (Performance::fromXml (*xml));
        }
        else if (target == "user")
        {
            const int u = v.hasProperty ("index") ? (int) v["index"] : remoteUser;
            if (u < 0 || u >= proc.getNumUsers()) return;
            auto xml = proc.getUser (u).toXml();
            bool any = false;
            for (auto& s : *sets)
                if (s.isArray() && s.size() >= 2) any = setPath (*xml, s[0].toString(), s[1]) || any;
            if (any) proc.updateUser (u, UserProgram::fromXml (*xml));
        }
    }
    else if (op == "editUser")   { remoteUser = juce::jlimit (-1, proc.getNumUsers() - 1, (int) v["index"]); dirty = true; }
    else if (op == "store")      { if (proc.getCurrentSlot() >= 0) proc.storePerformance (proc.getCurrentSlot()); }
    else if (op == "recall")     { const int s = (int) v["slot"]; if (s >= 0 && s < kNumPerfSlots) proc.recallPerformance (s); }
    else if (op == "step")       proc.stepPerformance ((int) v["dir"]);
    else if (op == "harmony")    proc.setTercaOn ((bool) v["on"]);
    else if (op == "vector")     proc.setVectorXY ((float) (double) v["x"], (float) (double) v["y"]);
    else if (op == "gvol")       proc.setGlobalVolume ((float) (double) v["db"]);
    else if (op == "geq")
    {
        auto g = proc.getGlobalEq();
        if (v["k"].toString() == "flat") { for (auto& x : g.g) x = 0.0f; g.lowDb = g.m1Db = g.m2Db = g.highDb = 0.0f; g.hpfHz = 20.0f; }
        else g.setByName (v["k"].toString(), (float) (double) v["v"]);
        proc.setGlobalEq (g);
    }
    else if (op == "panic")      proc.panic();
    else if (op == "perfName")   { auto p = proc.getPerformance(); p.name = v["name"].toString().trim().substring (0, 40); if (p.name.isNotEmpty()) proc.updatePerformance (p); }
    // ---------- liste performansa ----------
    else if (op == "bank")       { proc.setCurrentBank (juce::jlimit (0, kNumPerfBanks - 1, (int) v["bank"])); dirty = true; }
    else if (op == "bankName")   { proc.setBankName (juce::jlimit (0, kNumPerfBanks - 1, (int) v["bank"]), v["name"].toString().trim()); dirty = true; }
    else if (op == "storeTo")    { const int s = (int) v["slot"]; if (s >= 0 && s < kNumPerfSlots) proc.storePerformance (s); }
    else if (op == "clearSlot")  { const int s = (int) v["slot"]; if (s >= 0 && s < kNumPerfSlots && s != proc.getCurrentSlot()) proc.clearPerformanceSlot (s); dirty = true; }
    else if (op == "slotCat")    { const int s = (int) v["slot"]; if (s >= 0 && s < kNumPerfSlots) proc.setSlotCategory (s, (int) v["cat"]); dirty = true; }
    // ---------- set liste ----------
    else if (op == "setList")    { proc.setCurrentSetList (juce::jlimit (0, kNumSetLists - 1, (int) v["list"])); dirty = true; }
    else if (op == "setListPage"){ proc.setSetListPage ((int) v["page"]); dirty = true; }
    else if (op == "setListPlay"){ const int s = (int) v["slot"]; if (s >= 0 && s < kSetListSlots) proc.recallSetListSlot (s); }
    else if (op == "setListName"){ const int i = proc.getCurrentSetList(); auto sl = proc.getSetList (i); sl.name = v["name"].toString().trim(); proc.updateSetList (i, sl); dirty = true; }
    else if (op == "setListEdit")
    {
        const int s = (int) v["slot"];
        if (s < 0 || s >= kSetListSlots) return;
        const int i = proc.getCurrentSetList();
        auto sl = proc.getSetList (i);
        auto& x = sl.slots[(size_t) s];
        if (v.hasProperty ("clear")) x = SetListSlot();
        if (v.hasProperty ("perf"))   x.perf = juce::jlimit (-1, kNumPerfSlots - 1, (int) v["perf"]);
        if (v.hasProperty ("label"))  x.label = v["label"].toString().trim();
        if (v.hasProperty ("colour")) x.colour = juce::jlimit (0, 7, (int) v["colour"]);
        proc.updateSetList (i, sl);
        dirty = true;
    }
    else if (op == "pcMode")     { proc.setProgramChangeMode ((int) v["mode"]); dirty = true; }
    // ---------- harmony ----------
    else if (op == "tercaKey")   proc.setTercaKey (juce::jlimit (0, 11, (int) v["root"]), juce::jmax (0, (int) v["scale"]));
    else if (op == "tercaLearn") proc.startLearn (! proc.isLearning());
    else if (op == "terca")
    {
        // cela podesavanja (bez LEARN dugmeta, ono ostaje)
        const auto& d = v["data"];
        auto cur = proc.getTerca().toXml();
        juce::XmlElement x ("Terce");
        x.setAttribute ("voiceMode", juce::jlimit (0, 2, (int) d["voiceMode"]));
        x.setAttribute ("velocity", juce::jlimit (0.1, 1.5, (double) d["velocity"]));
        x.setAttribute ("topNoteOnly", (bool) d["topNoteOnly"]);
        for (auto* k : { "learnType", "learnChannel", "learnNumber" }) x.setAttribute (k, cur->getIntAttribute (k));
        if (auto* sc = d["Scale"].getArray())
            for (auto& s : *sc)
            {
                auto* e = x.createNewChildElement ("Scale");
                e->setAttribute ("name", s["name"].toString());
                e->setAttribute ("above", s["above"].toString());
                e->setAttribute ("below", s["below"].toString());
            }
        if (auto* rs = d["Rule"].getArray())
            for (auto& r : *rs)
            {
                auto* e = x.createNewChildElement ("Rule");
                e->setAttribute ("mask", (int) r["mask"] & 0xfff);
                e->setAttribute ("scale", (int) r["scale"]);
                e->setAttribute ("rootShift", juce::jlimit (-11, 11, (int) r["rootShift"]));
            }
        proc.updateTerca (TercaSettings::fromXml (x));
        dirty = true;
    }
    // ---------- useri ----------
    else if (op == "userDuplicate")
    {
        const int u = (int) v["index"];
        if (u < 0 || u >= proc.getNumUsers()) return;
        const auto copy = proc.getUser (u);
        remoteUser = proc.createUser (copy.name, &copy);
        dirty = true;
    }
    else if (op == "userDelete")
    {
        const int u = (int) v["index"];
        if (u < 0 || u >= proc.getNumUsers()) return;
        proc.deleteUser (u);
        remoteUser = juce::jmin (u, proc.getNumUsers() - 1);
        dirty = true;
    }
    else if (op == "userMove")
    {
        const int from = (int) v["from"], to = (int) v["to"];
        if (from < 0 || from >= proc.getNumUsers() || to < 0 || to >= proc.getNumUsers()) return;
        proc.moveUser (from, to);
        if (remoteUser == from) remoteUser = to;
        dirty = true;
    }
    else if (op == "userCat")    { proc.editUserCategory (v["old"].toString(), v["name"].toString().trim()); dirty = true; }
    // ---------- mapping ----------
    else if (op == "mapInst")
    {
        mapInst = v["name"].toString();
        mapUser = v.hasProperty ("user") ? (int) v["user"] : remoteUser;
        if (mapInst.isNotEmpty()) proc.ensureLoaded (mapInst);
        dirty = true;
    }
    else if (op == "mapZone")
    {
        auto inst = proc.getLoadedInstrument (mapInst);
        const int i = (int) v["index"];
        if (inst == nullptr || i < 0 || i >= (int) inst->zones.size()) return;
        auto zones = inst->zones;
        auto& z = zones[(size_t) i];
        const auto& f = v["set"];
        auto geti = [&] (const char* k, int& dst, int lo, int hi) { if (f.hasProperty (k)) dst = juce::jlimit (lo, hi, (int) f[k]); };
        geti ("lo", z.lowKey, 0, 127);  geti ("hi", z.highKey, 0, 127);
        geti ("lv", z.lowVel, 1, 127);  geti ("hv", z.highVel, 1, 127);
        geti ("root", z.rootKey, 0, 127);
        if (z.highKey < z.lowKey) std::swap (z.lowKey, z.highKey);
        if (z.highVel < z.lowVel) std::swap (z.lowVel, z.highVel);
        if (f.hasProperty ("tune")) z.tuneCents = (float) juce::jlimit (-100.0, 100.0, (double) f["tune"]);
        if (f.hasProperty ("gain")) z.gainDb = (float) juce::jlimit (-40.0, 12.0, (double) f["gain"]);
        if (f.hasProperty ("pan"))  z.pan = (float) juce::jlimit (-1.0, 1.0, (double) f["pan"]);
        if (f.hasProperty ("loop")) z.loop = (bool) f["loop"];
        if (f.hasProperty ("fixed")) z.fixedPitch = (bool) f["fixed"];
        if (f.hasProperty ("en"))   z.enabled = (bool) f["en"];
        proc.applyZones (mapInst, zones);
        mapDirty = true;
        dirty = true;
    }
    else if (op == "mapSave")    { if (mapInst.isNotEmpty() && proc.saveMap (mapInst)) mapDirty = false; dirty = true; }
    else if (op == "mapRevert")  { if (mapInst.isNotEmpty()) proc.reloadInstrument (mapInst, false); mapDirty = false; dirty = true; }
    else if (op == "mapDelete")  deleteMapZones (v);
    else if (op == "gtrans")      proc.setGlobalTranspose ((int) v["st"]);
    else if (op == "lock")        proc.setLiveLock ((bool) v["on"]);
    else if (op == "tuneOn")      proc.setTuning ((bool) v["on"]);
    else if (op == "tunePreset")  proc.setTuningPreset ((int) v["preset"], (int) v["root"]);
    else if (op == "tuneCents")   proc.setTuneCents ((int) v["pc"], (float) (double) v["cents"]);
    else if (op == "qtEdit")      proc.setQtEdit ((bool) v["on"]);
    else if (op == "gig")         proc.setGigMode ((bool) v["on"]);
    else if (op == "impulse")     proc.impulseFromRemote (v["id"].toString(), (int) v["v"]);
    else if (op == "maxPoly")     proc.setMaxPolyphony ((int) v["n"]);
    else if (op == "keyLearn")    { if ((int) v["mode"] == 0) proc.cancelKeyLearn(); else proc.armKeyLearn ((int) v["part"], (int) v["mode"]); }
    else if (op == "sceneRecall") proc.recallScene ((int) v["scene"]);
    else if (op == "sceneStore")  proc.storeScene ((int) v["scene"]);
    else if (op == "sceneClear")  proc.clearScene ((int) v["scene"]);
    else if (op == "sceneName")   proc.renameScene ((int) v["scene"], v["name"].toString());
    else if (op == "newFmUser")
    {
        remoteUser = proc.createFmUser();
        dirty = true;
    }
    else if (op == "fmAlg")
    {
        // DX algoritam menja i veze operatora (to radi Workstation, ne iPad)
        const int u = v.hasProperty ("index") ? (int) v["index"] : remoteUser;
        if (u < 0 || u >= proc.getNumUsers() || ! proc.getUser (u).isFm()) return;
        auto user = proc.getUser (u);
        auto pt = proc.fmPatchFor (user);
        if (pt == nullptr) return;
        auto p = *pt;
        p.applyDxAlgorithm (juce::jlimit (1, 32, (int) v["alg"]));
        user.fmPatch = p.toString();
        user.fmProgram = {};
        proc.updateUser (u, user);
    }
    else if (op == "newUser")
    {
        const auto inst = v["instrument"].toString();
        const int u = inst.isEmpty() ? proc.createUser ("User") : proc.createUserFromInstrument (inst);
        remoteUser = u;
        const int part = (int) v["part"];
        if (part >= 0 && part < kNumParts)
        {
            auto perf = proc.getPerformance();
            perf.parts[(size_t) part].user = proc.getUser (u).name;
            perf.parts[(size_t) part].on = true;
            proc.updatePerformance (perf);
        }
        dirty = true;
    }
}

// brise zone (kao TOOLS u MAPPING): neighbours = najblize zone preuzimaju tastere, files = WAV u Recycle Bin
void RemoteServer::deleteMapZones (const juce::var& v)
{
    auto inst = proc.getLoadedInstrument (mapInst);
    if (inst == nullptr) return;
    std::set<int> del;
    if (auto* a = v["zones"].getArray()) for (auto& x : *a) del.insert ((int) x);
    if (v.hasProperty ("from"))
        for (int i = 0; i < (int) inst->zones.size(); ++i)
        {
            const auto& z = inst->zones[(size_t) i];
            if (z.lowKey >= (int) v["from"] && z.highKey <= (int) v["to"]) del.insert (i);
        }
    std::vector<Zone> keep, gone;
    for (int i = 0; i < (int) inst->zones.size(); ++i) (del.count (i) ? gone : keep).push_back (inst->zones[(size_t) i]);
    if (gone.empty()) return;
    juce::Array<juce::File> files;
    if ((bool) v["files"] && inst->type == SourceType::WavFolder)
        for (auto& g : gone)
        {
            if (g.sample < 0 || g.sample >= (int) inst->samples.size()) continue;
            bool used = false;
            for (auto& k : keep) used = used || k.sample == g.sample;
            if (! used) files.addIfNotAlreadyThere (inst->source.getChildFile (inst->samples[(size_t) g.sample]->id));
        }
    if ((bool) v["neighbours"])
        for (auto& g : gone)
            for (int key = g.lowKey; key <= g.highKey; ++key)
            {
                int best = -1, bestDist = 1000;
                for (int k = 0; k < (int) keep.size(); ++k)
                {
                    const auto& z = keep[(size_t) k];
                    if (! z.enabled || z.highVel < g.lowVel || z.lowVel > g.highVel) continue;
                    if (key >= z.lowKey && key <= z.highKey) { best = -1; break; }
                    const int d = key < z.lowKey ? z.lowKey - key : key - z.highKey;
                    if (d < bestDist) { bestDist = d; best = k; }
                }
                if (best >= 0) { auto& z = keep[(size_t) best]; z.lowKey = juce::jmin (z.lowKey, key); z.highKey = juce::jmax (z.highKey, key); }
            }
    proc.applyZones (mapInst, keep);
    proc.saveMap (mapInst);
    for (auto& f : files) if (! f.moveToTrash()) f.deleteFile();
    proc.reloadInstrument (mapInst, false);
    mapDirty = false;
    dirty = true;
}

//==============================================================================
juce::StringPairArray RemoteServer::buildSections()
{
    juce::StringPairArray out;
    {
        auto* st = new juce::DynamicObject();
        const int slot = proc.getCurrentSlot();
        st->setProperty ("slot", slot);
        st->setProperty ("slotText", slot >= 0 ? juce::String (slot / kPerfPerBank + 1) + "-" + juce::String (slot % kPerfPerBank).paddedLeft ('0', 3) : juce::String ("---"));
        st->setProperty ("list", slot >= 0 ? proc.getBankName (slot / kPerfPerBank) : proc.getBankName (proc.getCurrentBank()));
        st->setProperty ("name", proc.getPerformance().name);
        st->setProperty ("dirty", proc.isPerformanceDirty());
        st->setProperty ("harm", proc.isTercaOn());
        st->setProperty ("harmText", proc.getTercaText());
        st->setProperty ("voices", proc.getActiveVoices());
        st->setProperty ("gvol", proc.getGlobalVolume());
        {
            const auto& g = proc.getGlobalEq();
            auto* e = new juce::DynamicObject();
            for (auto& k : GlobalEqSettings::names()) e->setProperty (k, g.getByName (k));
            st->setProperty ("geq", juce::var (e));
        }
        st->setProperty ("vx", std::round (proc.getVectorX() * 100.0f) / 100.0f);
        st->setProperty ("vy", std::round (proc.getVectorY() * 100.0f) / 100.0f);
        st->setProperty ("c3", proc.getC3Is60());
        st->setProperty ("gtrans", proc.getGlobalTranspose());
        st->setProperty ("locked", proc.isLiveLocked());
        {
            const auto& pf = proc.getPerformance();
            st->setProperty ("tuneOn", pf.tuneOn);
            st->setProperty ("tuneName", pf.tuneName);
            st->setProperty ("qtEdit", proc.isQtEdit());
            st->setProperty ("gig", proc.isGigMode());
            {
                // IMPULSE: [id, vrsta, ime, vrednost, sta radi u Workstation-u, stanje -1/0/1]
                juce::Array<juce::var> im;
                for (auto& c : proc.getImpulseLayout())
                {
                    int state = -1;
                    const auto what = proc.impulseTargetText (c, state);
                    im.add (juce::Array<juce::var> { c.id, c.kind, c.name, proc.getImpulseValue (c), what, state });
                }
                st->setProperty ("imp", im);
                st->setProperty ("impSeen", proc.isImpulseSeen());
                st->setProperty ("impCh", proc.getImpulseChannel());
            }
            st->setProperty ("keyLearn", proc.getKeyLearnMode());
            const auto ai = proc.getAudioInfo();
            st->setProperty ("cpu", juce::roundToInt (proc.getAudioLoad() * 100.0f));
            st->setProperty ("latency", std::round (ai.latencyMs * 10.0) / 10.0);
            st->setProperty ("driver", ai.driver);
            st->setProperty ("audioWarn", ai.warn);
            st->setProperty ("audioAdvice", ai.advice);
            juce::Array<juce::var> tc, tp;
            for (auto c : pf.tuneCents) tc.add (std::round (c * 10.0f) / 10.0f);
            for (auto& t : Tuning::presets()) tp.add (juce::String (t.name));
            st->setProperty ("tuneCents", tc);
            st->setProperty ("tunePresets", tp);
        }
        st->setProperty ("status", proc.getStatusText());
        int ru = remoteUser;
        if (ru < 0) ru = proc.findUser (proc.getPerformance().parts[0].user);
        st->setProperty ("remoteUser", ru);
        out.set ("status", json (juce::var (st)));
    }
    if (dirty || lastSent.size() == 0)
    {
        if (auto xml = proc.getPerformance().toXml())
            out.set ("perf", json (xmlToVar (*xml)));

        juce::Array<juce::var> list;
        for (int i = 0; i < proc.getNumUsers(); ++i)
        {
            const auto& u = proc.getUser (i);
            list.add (juce::Array<juce::var> { u.name, u.category });
        }
        juce::Array<juce::var> cats;
        for (auto& c : proc.getUserCategories()) cats.add (c);
        auto* us = new juce::DynamicObject();
        us->setProperty ("list", list);
        us->setProperty ("cats", cats);
        out.set ("users", json (juce::var (us)));

        int ru = remoteUser;
        if (ru < 0) ru = proc.findUser (proc.getPerformance().parts[0].user);
        auto* ud = new juce::DynamicObject();
        ud->setProperty ("index", ru);
        if (ru >= 0 && ru < proc.getNumUsers())
            if (auto xml = proc.getUser (ru).toXml())
            {
                // FM user uvezen pre editora (samo MOD-7 bajtovi): iPad dobija parametre kao fmPatch
                const auto& us = proc.getUser (ru);
                if (us.isFm() && us.fmPatch.isEmpty())
                    if (auto pt = proc.fmPatchFor (us)) xml->setAttribute ("fmPatch", pt->toString());
                xml->removeAttribute ("fmProgram");   // veliki hex, iPadu ne treba
                ud->setProperty ("data", xmlToVar (*xml));
            }
        out.set ("user", json (juce::var (ud)));

        const auto& inst = proc.getInstrumentList();
        const auto ver = juce::String (inst.size()) + ":" + juce::String (inst.joinIntoString ("|").hashCode64());
        auto* meta = new juce::DynamicObject();
        meta->setProperty ("instVer", ver);
        juce::Array<juce::var> fx;
        for (auto& n : InsertFx::getTypeNames()) fx.add (n);
        meta->setProperty ("fx", fx);
        out.set ("meta", json (juce::var (meta)));
        if (ver != instrumentsVer)
        {
            juce::Array<juce::var> a;
            for (auto& n : inst) a.add (n);
            const auto j = json (juce::var (a));
            const juce::ScopedLock sl (clientsLock);
            instrumentsJson = j;
            instrumentsVer = ver;
        }
        // ---------- liste performansa ----------
        {
            auto* li = new juce::DynamicObject();
            li->setProperty ("bank", proc.getCurrentBank());
            li->setProperty ("cur", proc.getCurrentSlot());
            juce::Array<juce::var> banks, cats, slots;
            for (int b = 0; b < kNumPerfBanks; ++b) banks.add (proc.getBankName (b));
            for (int k = 0; k < proc.getCategories().size(); ++k)
                cats.add (juce::Array<juce::var> { proc.getCategories()[k], "#" + proc.categoryColour (k).toDisplayString (false) });
            for (int s = 0; s < kNumPerfSlots; ++s)
            {
                const auto& p = proc.getSlot (s);
                if (p.used) slots.add (juce::Array<juce::var> { p.name, proc.categoryIndex (p) });
                else slots.add (0);
            }
            li->setProperty ("banks", banks);
            li->setProperty ("cats", cats);
            li->setProperty ("slots", slots);
            out.set ("lists", json (juce::var (li)));
        }
        // ---------- set lista ----------
        {
            auto* sl = new juce::DynamicObject();
            const int cur = proc.getCurrentSetList();
            sl->setProperty ("list", cur);
            sl->setProperty ("page", proc.getSetListPage());
            sl->setProperty ("slot", proc.getCurrentSetListSlot());
            sl->setProperty ("pcMode", proc.getProgramChangeMode());
            juce::Array<juce::var> names, slots;
            for (int i = 0; i < kNumSetLists; ++i)
            {
                const auto n = proc.getSetList (i).name;
                names.add (n.isNotEmpty() ? n : "Set list " + juce::String (i + 1));
            }
            for (auto& x : proc.getSetList (cur).slots)
            {
                const auto& p = proc.getSlot (x.perf >= 0 ? x.perf : 0);
                const bool ok = x.perf >= 0 && p.used;
                slots.add (juce::Array<juce::var> { x.label.isNotEmpty() ? x.label : (ok ? p.name : juce::String()), x.colour, x.perf, ok });
            }
            sl->setProperty ("names", names);
            sl->setProperty ("slots", slots);
            out.set ("setlist", json (juce::var (sl)));
        }
        // ---------- harmony ----------
        {
            const auto& t = proc.getTerca();
            auto var = xmlToVar (*t.toXml());
            if (auto* o = var.getDynamicObject())
            {
                o->setProperty ("on", proc.isTercaOn());
                o->setProperty ("root", proc.getTercaRoot());
                o->setProperty ("scale", proc.getTercaScale());
                o->setProperty ("text", proc.getTercaText());
                o->setProperty ("learning", proc.isLearning());
                o->setProperty ("learnText", t.learn.describe());
            }
            out.set ("terca", json (var));
        }
        // ---------- mapping ----------
        {
            auto* mp = new juce::DynamicObject();
            mp->setProperty ("name", mapInst);
            mp->setProperty ("dirty", mapDirty);
            mp->setProperty ("c3", proc.getC3Is60());
            // multisemplovi usera koji se edituje (brz izbor)
            juce::Array<juce::var> mine;
            const int mu = mapUser >= 0 ? mapUser : remoteUser;
            if (mu >= 0 && mu < proc.getNumUsers())
                for (auto& o : proc.getUser (mu).osc)
                    if (o.instrument.isNotEmpty() && ! mine.contains (o.instrument)) mine.add (o.instrument);
            mp->setProperty ("userInst", mine);
            if (auto inst = mapInst.isNotEmpty() ? proc.getLoadedInstrument (mapInst) : nullptr)
            {
                mp->setProperty ("loaded", true);
                mp->setProperty ("wav", inst->type == SourceType::WavFolder);
                juce::Array<juce::var> zs;
                for (auto& z : inst->zones)
                {
                    auto* o = new juce::DynamicObject();
                    o->setProperty ("s", z.sample >= 0 && z.sample < (int) inst->samples.size() ? inst->samples[(size_t) z.sample]->id : juce::String());
                    o->setProperty ("lo", z.lowKey);  o->setProperty ("hi", z.highKey);
                    o->setProperty ("lv", z.lowVel);  o->setProperty ("hv", z.highVel);
                    o->setProperty ("root", z.rootKey);
                    o->setProperty ("tune", z.tuneCents); o->setProperty ("gain", z.gainDb); o->setProperty ("pan", z.pan);
                    o->setProperty ("loop", z.loop); o->setProperty ("fixed", z.fixedPitch); o->setProperty ("en", z.enabled);
                    zs.add (juce::var (o));
                }
                mp->setProperty ("zones", zs);
            }
            else mp->setProperty ("loaded", false);
            out.set ("map", json (juce::var (mp)));
        }
        dirty = false;
    }
    return out;
}

void RemoteServer::timerCallback()
{
    std::vector<std::shared_ptr<Client>> cs;
    {
        const juce::ScopedLock sl (clientsLock);
        cs = clients;
    }
    if (cs.empty()) { lastSent.clear(); dirty = true; return; }

    bool anyNew = false;
    for (auto& c : cs) anyNew = anyNew || c->needFull;
    if (anyNew) dirty = true;
    auto now = buildSections();

    // ono sto se promenilo od poslednjeg slanja
    juce::String changed;
    for (auto& key : now.getAllKeys())
    {
        const auto val = now[key];
        const auto part = "\"" + key + "\":" + val;
        if (lastSent[key] != val) changed << (changed.isEmpty() ? "" : ",") << part;
        lastSent.set (key, val);
    }
    for (auto& c : cs)
    {
        juce::String msg;
        if (c->needFull)
        {
            // novi klijent dobija sve, i delove koji se nisu menjali ovaj put
            juce::String all;
            for (auto& key : lastSent.getAllKeys())
                all << (all.isEmpty() ? "" : ",") << "\"" << key << "\":" << lastSent[key];
            msg = "{" + all + "}";
            c->needFull = false;
        }
        else if (changed.isNotEmpty())
            msg = "{" + changed + "}";
        if (msg.isEmpty()) continue;
        {
            const juce::ScopedLock sl (c->lock);
            if (c->queue.size() > 50) { c->queue.clear(); c->needFull = true; }   // klijent ne stize da cita: sledeci put sve iz pocetka
            else c->queue.add (msg);
        }
        c->ready.signal();
    }
}

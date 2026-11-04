#include "RemoteServer.h"
#include "PluginProcessor.h"
#include "SamplerPart.h"
#include "BinaryData.h"
#include <thread>
#if ! JUCE_WINDOWS
 #include <csignal>
#endif

namespace
{
    // XML (kao u podacima) -> JSON: atributi su polja, deca su nizovi po imenu taga
    juce::var xmlToVar (const juce::XmlElement& x)
    {
        static const juce::StringArray textKeys { "name", "user", "instrument", "category", "categoryName", "kronosSource", "kronosPcg" };
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
    else if (op == "step")
    {
        // sledeci / prethodni zauzet performans u istoj listi
        const int dir = (int) v["dir"] >= 0 ? 1 : -1;
        int s = proc.getCurrentSlot();
        const int bank = s >= 0 ? s / kPerfPerBank : proc.getCurrentBank();
        if (s < 0) s = bank * kPerfPerBank - dir;
        for (int k = 0; k < kPerfPerBank; ++k)
        {
            s += dir;
            if (s < bank * kPerfPerBank || s >= (bank + 1) * kPerfPerBank) break;
            if (proc.getSlot (s).used) { proc.recallPerformance (s); break; }
        }
    }
    else if (op == "harmony")    proc.setTercaOn ((bool) v["on"]);
    else if (op == "vector")     proc.setVectorXY ((float) (double) v["x"], (float) (double) v["y"]);
    else if (op == "gvol")       proc.setGlobalVolume ((float) (double) v["db"]);
    else if (op == "panic")      proc.panic();
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

//==============================================================================
juce::StringPairArray RemoteServer::buildSections()
{
    juce::StringPairArray out;
    {
        auto* st = new juce::DynamicObject();
        const int slot = proc.getCurrentSlot();
        st->setProperty ("slot", slot);
        st->setProperty ("slotText", slot >= 0 ? juce::String (slot / kPerfPerBank + 1) + "-" + juce::String (slot % kPerfPerBank + 1).paddedLeft ('0', 3) : juce::String ("---"));
        st->setProperty ("list", slot >= 0 ? proc.getBankName (slot / kPerfPerBank) : proc.getBankName (proc.getCurrentBank()));
        st->setProperty ("name", proc.getPerformance().name);
        st->setProperty ("dirty", proc.isPerformanceDirty());
        st->setProperty ("harm", proc.isTercaOn());
        st->setProperty ("harmText", proc.getTercaText());
        st->setProperty ("voices", proc.getActiveVoices());
        st->setProperty ("gvol", proc.getGlobalVolume());
        st->setProperty ("vx", std::round (proc.getVectorX() * 100.0f) / 100.0f);
        st->setProperty ("vy", std::round (proc.getVectorY() * 100.0f) / 100.0f);
        st->setProperty ("c3", proc.getC3Is60());
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
                ud->setProperty ("data", xmlToVar (*xml));
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

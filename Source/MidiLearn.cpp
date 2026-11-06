#include "MidiLearn.h"

MidiLearn* MidiLearn::instance = nullptr;

MidiLearn& MidiLearn::get()
{
    // DeletedAtShutdown: obrise se pre gasenja JUCE-a
    if (instance == nullptr) instance = new MidiLearn();
    return *instance;
}

void MidiLearn::removeAllTargets() { targets.clear(); }

MidiLearn::MidiLearn()
{
    for (auto& a : table) for (auto& b : a) for (auto& c : b) c = 0;
    load();
    startTimerHz (60);
}

MidiLearn::~MidiLearn() { stopTimer(); instance = nullptr; }

juce::String MidiLearn::Assign::describe() const
{
    if (type == 1) return "CC " + juce::String (number) + " (ch " + juce::String (channel) + ")";
    if (type == 2) return "Key " + juce::MidiMessage::getMidiNoteName (number, true, true, 3) + " (ch " + juce::String (channel) + ")";
    return {};
}

juce::File MidiLearn::getFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("LiveSampler").getChildFile ("midi-learn.xml");
}

void MidiLearn::load()
{
    assigns.clear();
    if (auto xml = juce::XmlDocument::parse (getFile()))
        for (auto* e : xml->getChildWithTagNameIterator ("Learn"))
        {
            Assign a;
            a.type = e->getIntAttribute ("type");
            a.channel = juce::jlimit (1, 16, e->getIntAttribute ("channel", 1));
            a.number = juce::jlimit (0, 127, e->getIntAttribute ("number"));
            const auto id = e->getStringAttribute ("id");
            if (id.isNotEmpty() && (a.type == 1 || a.type == 2)) assigns[id] = a;
        }
    rebuildTable();
}

void MidiLearn::save() const
{
    juce::XmlElement root ("MidiLearn");
    for (auto& kv : assigns)
    {
        auto* e = root.createNewChildElement ("Learn");
        e->setAttribute ("id", kv.first);
        e->setAttribute ("type", kv.second.type);
        e->setAttribute ("channel", kv.second.channel);
        e->setAttribute ("number", kv.second.number);
    }
    getFile().getParentDirectory().createDirectory();
    root.writeTo (getFile());
}

void MidiLearn::rebuildTable()
{
    for (auto& a : table) for (auto& b : a) for (auto& c : b) c = 0;
    for (auto& kv : assigns)
        table[kv.second.type == 1 ? 0 : 1][kv.second.channel - 1][kv.second.number] = 1;
}

void MidiLearn::addTarget (const juce::String& id, Target t) { targets[id] = std::move (t); }
void MidiLearn::removeTarget (const juce::String& id) { targets.erase (id); }

void MidiLearn::addKnob (const juce::String& id, double lo, double hi, double centre, std::function<void (double)> set)
{
    juce::NormalisableRange<double> range (lo, hi);
    if (centre > lo && centre < hi) range.setSkewForCentre (centre);
    Target t;
    t.setNorm = [range, set] (float v) { set (range.convertFrom0to1 ((double) juce::jlimit (0.0f, 1.0f, v))); };
    addTarget (id, std::move (t));
}

void MidiLearn::addToggle (const juce::String& id, std::function<bool()> getFn, std::function<void (bool)> setFn)
{
    Target t;
    t.isButton = true;
    t.getState = std::move (getFn);
    t.setState = std::move (setFn);
    addTarget (id, std::move (t));
}

void MidiLearn::bindSlider (juce::Slider& s, const juce::String& id)
{
    juce::Component::SafePointer<juce::Slider> safe (&s);
    Target t;
    t.setNorm = [safe] (float v)
    {
        if (safe == nullptr || safe->isMouseButtonDown()) return;
        safe->setValue (safe->proportionOfLengthToValue (juce::jlimit (0.0, 1.0, (double) v)), juce::sendNotificationSync);
    };
    addTarget (id, std::move (t));
    if (auto* ls = dynamic_cast<LearnSlider*> (&s)) ls->setLearnId (id);
}

void MidiLearn::bindButton (juce::Button& b, const juce::String& id)
{
    juce::Component::SafePointer<juce::Button> safe (&b);
    Target t;
    t.isButton = true;
    if (b.getClickingTogglesState())
    {
        t.getState = [safe] { return safe != nullptr && safe->getToggleState(); };
        t.setState = [safe] (bool on) { if (safe != nullptr && safe->isEnabled()) safe->setToggleState (on, juce::sendNotificationSync); };
    }
    else
    {
        t.trigger = [safe] { if (safe != nullptr && safe->isEnabled()) safe->triggerClick(); };
    }
    addTarget (id, std::move (t));
    if (auto* lb = dynamic_cast<LearnButton*> (&b)) lb->setLearnId (id);
}

void MidiLearn::addHoldButton (const juce::String& id, int holdMs, std::function<void()> action, juce::Component* forMark)
{
    Target t;
    t.isButton = true;
    t.trigger = std::move (action);
    t.holdMs = juce::jmax (100, holdMs);
    addTarget (id, std::move (t));
    if (auto* lb = dynamic_cast<LearnButton*> (forMark)) lb->setLearnId (id);
}

void MidiLearn::checkHolds()
{
    if (holdStart.empty()) return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (auto it = holdStart.begin(); it != holdStart.end();)
    {
        auto t = targets.find (it->first);
        auto a = assigns.find (it->first);
        if (t == targets.end() || a == assigns.end()) { it = holdStart.erase (it); continue; }
        if (now - it->second >= (double) t->second.holdMs)
        {
            holdFired.insert ((a->second.type * 16 + a->second.channel) * 128 + a->second.number);
            auto fn = t->second.trigger;
            it = holdStart.erase (it);
            if (fn) fn();
        }
        else ++it;
    }
}

void MidiLearn::bindCombo (juce::ComboBox& c, const juce::String& id)
{
    juce::Component::SafePointer<juce::ComboBox> safe (&c);
    Target t;
    t.setNorm = [safe] (float v)
    {
        if (safe == nullptr || safe->getNumItems() == 0) return;
        const int n = safe->getNumItems();
        const int idx = juce::jlimit (0, n - 1, (int) std::floor (v * (float) n * 0.9999f));
        if (idx != safe->getSelectedItemIndex()) safe->setSelectedItemIndex (idx, juce::sendNotificationSync);
    };
    addTarget (id, std::move (t));
    if (auto* lc = dynamic_cast<LearnCombo*> (&c)) lc->setLearnId (id);
}

juce::String MidiLearn::describe (const juce::String& id) const
{
    auto it = assigns.find (id);
    return it == assigns.end() ? juce::String() : it->second.describe();
}

void MidiLearn::clear (const juce::String& id)
{
    if (assigns.erase (id) > 0)
    {
        rebuildTable();
        save();
    }
    sendChangeMessage();
}

void MidiLearn::startLearn (const juce::String& id, bool allowNote)
{
    learnTargetId = id;
    learnAllowNote = allowNote;
    learnMode = allowNote ? 2 : 1;
    sendChangeMessage();
}

void MidiLearn::cancelLearn()
{
    learnMode = 0;
    learnTargetId = {};
    sendChangeMessage();
}

void MidiLearn::showMenu (juce::Component* target, const juce::String& id, bool allowNote,
                          std::function<void (juce::PopupMenu&)> extraItems, std::function<void (int)> onExtra)
{
    if (id.isEmpty()) return;
    juce::PopupMenu m;
    const auto cur = describe (id);
    m.addSectionHeader (cur.isNotEmpty() ? "Learned: " + cur : juce::String ("Not learned"));
    if (isLearning() && learnTargetId == id)
        m.addItem (3, "Cancel MIDI Learn");
    else
        m.addItem (1, allowNote ? "MIDI Learn (move a CC or press a key)" : "MIDI Learn (move a knob / CC)");
    m.addItem (2, "Remove Learn", cur.isNotEmpty());
    if (extraItems) { m.addSeparator(); extraItems (m); }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [this, id, allowNote, onExtra] (int r)
    {
        if (r >= 100) { if (onExtra) onExtra (r); return; }
        if (r == 1) startLearn (id, allowNote);
        else if (r == 2) clear (id);
        else if (r == 3) cancelLearn();
    });
}

bool MidiLearn::handleIncoming (const juce::MidiMessage& m)
{
    const int mode = learnMode.load();
    const bool isCC = m.isController();
    const bool isNote = m.isNoteOnOrOff();
    if (! isCC && ! isNote) return false;
    const int ch = juce::jlimit (1, 16, m.getChannel());
    const int num = isCC ? m.getControllerNumber() : m.getNoteNumber();
    if (isCC && (num == 0 || num == 32 || num >= 120)) return false;   // bank select, channel mode messages

    if (mode != 0)
    {
        const bool take = (isCC && m.getControllerValue() > 0) || (mode == 2 && m.isNoteOn());
        if (take)
        {
            learnMode = 0;
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) events[s1] = { (juce::uint8) (isCC ? 1 : 2), (juce::uint8) ch, (juce::uint8) num, (juce::uint8) 0, true };
            fifo.finishedWrite (n1);
            return true;
        }
    }

    if (table[isCC ? 0 : 1][ch - 1][num].load() == 0) return false;
    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)
    {
        const int value = isCC ? m.getControllerValue() : (m.isNoteOn() ? 127 : 0);
        events[s1] = { (juce::uint8) (isCC ? 1 : 2), (juce::uint8) ch, (juce::uint8) num, (juce::uint8) value, false };
    }
    fifo.finishedWrite (n1);
    return true;
}

void MidiLearn::timerCallback()
{
    // blink the control that is waiting for MIDI
    if (isLearning() && ++blinkTick % 18 == 0) sendChangeMessage();
    checkHolds();
    const int ready = fifo.getNumReady();
    if (ready == 0) return;
    int s1, n1, s2, n2;
    fifo.prepareToRead (ready, s1, n1, s2, n2);
    std::vector<Event> evs;
    for (int i = 0; i < n1; ++i) evs.push_back (events[s1 + i]);
    for (int i = 0; i < n2; ++i) evs.push_back (events[s2 + i]);
    fifo.finishedRead (n1 + n2);

    // knobs: only the last value of each CC in this round (smooth and light)
    std::map<int, int> lastCC;
    for (size_t i = 0; i < evs.size(); ++i)
        if (evs[i].type == 1 && ! evs[i].learn) lastCC[evs[i].channel * 128 + evs[i].number] = (int) i;

    for (size_t i = 0; i < evs.size(); ++i)
    {
        const auto& e = evs[i];
        if (e.learn)
        {
            if (learnTargetId.isEmpty()) continue;
            Assign a;
            a.type = e.type;
            a.channel = e.channel;
            a.number = e.number;
            // one control per CC / key: the new one takes it over. Izuzetak: hold dugme (drzi se dugo) i obicno dugme
            // mogu da dele isti taster (kratko = obicno, dugo = hold)
            auto isHold = [this] (const juce::String& id) { auto t = targets.find (id); return t != targets.end() && t->second.holdMs > 0; };
            const bool newHold = isHold (learnTargetId);
            for (auto it = assigns.begin(); it != assigns.end();)
                if (it->second.type == a.type && it->second.channel == a.channel && it->second.number == a.number
                    && (isHold (it->first) == newHold || it->first == learnTargetId)) it = assigns.erase (it);
                else ++it;
            assigns[learnTargetId] = a;
            learnTargetId = {};
            rebuildTable();
            save();
            sendChangeMessage();
            continue;
        }
        const int key = e.channel * 128 + e.number;
        if (e.type == 1)
        {
            // a CC on a button is handled for each message (press / release); on a knob only the last value
            bool anyButton = false;
            for (auto& kv : assigns)
                if (kv.second.type == 1 && kv.second.channel == e.channel && kv.second.number == e.number)
                {
                    auto t = targets.find (kv.first);
                    anyButton = anyButton || (t != targets.end() && t->second.isButton);
                }
            if (! anyButton && lastCC[key] != (int) i) continue;
        }
        dispatch (e.type, e.channel, e.number, e.value);
    }
}

juce::StringArray MidiLearn::idsFor (int type, int channel, int number) const
{
    juce::StringArray out;
    for (auto& kv : assigns)
        if (kv.second.type == type && kv.second.channel == channel && kv.second.number == number && targets.count (kv.first) > 0)
            out.add (kv.first);
    return out;
}

bool MidiLearn::stateOf (const juce::String& id, bool& state) const
{
    auto it = targets.find (id);
    if (it == targets.end() || ! it->second.isButton || ! it->second.getState) return false;
    state = it->second.getState();
    return true;
}

void MidiLearn::dispatch (int type, int channel, int number, int value)
{
    const int key = (type * 16 + channel) * 128 + number;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool latching = type == 1 && isLatching && isLatching (channel, number);
    // hold dugmad na ovom tasteru (ne rade za taster koji drzi stanje: on se ne moze drzati)
    int holdMs = 0;
    if (! latching)
        for (auto& kv : assigns)
            if (kv.second.type == type && kv.second.channel == channel && kv.second.number == number)
                if (auto t = targets.find (kv.first); t != targets.end() && t->second.holdMs > 0)
                {
                    holdMs = holdMs == 0 ? t->second.holdMs : juce::jmin (holdMs, t->second.holdMs);
                    if (value >= 64) holdStart[kv.first] = now; else holdStart.erase (kv.first);
                }
    // obicna dugmad koja dele taster sa hold dugmetom: kratak pritisak se okida pri pustanju
    bool shortPress = false;
    if (holdMs > 0)
    {
        if (value >= 64)
        {
            // dupla poruka (isti pritisak stigao dvaput) ne pocinje novo merenje
            if (deferStart.count (key) == 0) { deferStart[key] = now; holdFired.erase (key); }
            return;
        }
        const auto d = deferStart.find (key);
        if (d == deferStart.end()) return;
        shortPress = holdFired.count (key) == 0 && now - d->second < (double) holdMs;
        deferStart.erase (d);
        holdFired.erase (key);
        if (! shortPress) return;
    }
    for (auto& kv : assigns)
    {
        const auto& a = kv.second;
        if (a.type != type || a.channel != channel || a.number != number) continue;
        auto it = targets.find (kv.first);
        if (it == targets.end()) continue;
        auto& t = it->second;
        if (t.holdMs > 0) continue;   // hold dugme se okida u checkHolds
        if (shortPress && ! t.isButton) continue;
        if (shortPress && t.isButton)
        {
            if (t.trigger) t.trigger();
            else if (t.getState && t.setState) t.setState (! t.getState());
            continue;
        }
        if (! t.isButton)
        {
            if (type == 1 && t.setNorm) t.setNorm ((float) value / 127.0f);
            continue;
        }
        // ista poruka dvaput za redom (isti CC i vrednost u < 40 ms, npr. stigla i direktno i prosledjena preko drugog porta): samo jednom
        if (t.isButton)
        {
            const int dk = key * 128 + value;
            if (auto d = lastDup.find (dk); d != lastDup.end() && now - d->second < 40.0) continue;
            lastDup[dk] = now;
        }
        // dugme koje drzi stanje (Impulse toggle): svaka poruka je pritisak, odmah (i kad se brzo pritiska)
        if (type == 1 && isLatching && isLatching (channel, number))
        {
            if (t.trigger) t.trigger();
            else if (t.getState && t.setState) t.setState (! t.getState());
            continue;
        }
        // button: press toggles. A release that comes quickly (momentary pad / key) does nothing;
        // a "0" that comes later (latching switch on iPad) turns it off.
        if (value >= 64)
        {
            lastPress[key] = now;
            if (t.trigger) t.trigger();
            else if (t.getState && t.setState) t.setState (! t.getState());
        }
        else if (type == 1)
        {
            // "0" koji dodje kasnije = dugme koje drzi stanje (Impulse u toggle modu, iPad prekidac): to je novi pritisak.
            // Dugme sa stanjem se gasi; dugme bez stanja (HARMONY, PERF, scene...) se okida ponovo, kao drugi pritisak tastera.
            const bool momentary = lastPress.count (key) > 0 && now - lastPress[key] < 600.0;
            if (! momentary)
            {
                if (t.getState && t.setState) { if (t.getState()) t.setState (false); }
                else if (t.trigger) t.trigger();
            }
        }
    }
}

//==============================================================================
void LearnMark::paint (juce::Graphics& g, juce::Component& c, const juce::String& id)
{
    if (id.isEmpty()) return;
    auto& ml = MidiLearn::get();
    if (ml.isLearning() && ml.learningId() == id)
    {
        const bool blink = (juce::Time::getMillisecondCounter() / 300) % 2 == 0;
        g.setColour (juce::Colour (0xffffb340).withAlpha (blink ? 1.0f : 0.5f));
        g.drawRoundedRectangle (c.getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.5f);
    }
    else if (ml.isAssigned (id))
    {
        g.setColour (juce::Colour (0xff3dc5ff));
        g.fillEllipse ((float) c.getWidth() - 7.0f, 2.0f, 5.0f, 5.0f);
    }
}

static bool learnPopup (const juce::MouseEvent& e, const std::function<juce::String()>& id)
{
    return id && e.mods.isPopupMenu() && id().isNotEmpty();
}

void LearnSlider::mouseDown (const juce::MouseEvent& e)
{
    if (learnPopup (e, learnId)) { menuOpen = true; MidiLearn::get().showMenu (this, learnId(), false); return; }
    menuOpen = false;
    juce::Slider::mouseDown (e);
}
void LearnSlider::mouseDrag (const juce::MouseEvent& e) { if (! menuOpen) juce::Slider::mouseDrag (e); }
void LearnSlider::mouseUp (const juce::MouseEvent& e)   { if (menuOpen) { menuOpen = false; return; } juce::Slider::mouseUp (e); }

void LearnButton::mouseDown (const juce::MouseEvent& e)
{
    if (learnPopup (e, learnId)) { menuOpen = true; MidiLearn::get().showMenu (this, learnId(), true, extraMenuItems, onExtraMenuItem); return; }
    menuOpen = false;
    juce::TextButton::mouseDown (e);
}
void LearnButton::mouseUp (const juce::MouseEvent& e) { if (menuOpen) { menuOpen = false; return; } juce::TextButton::mouseUp (e); }

void LearnCombo::mouseDown (const juce::MouseEvent& e)
{
    if (learnPopup (e, learnId)) { menuOpen = true; MidiLearn::get().showMenu (this, learnId(), false); return; }
    menuOpen = false;
    juce::ComboBox::mouseDown (e);
}
void LearnCombo::mouseUp (const juce::MouseEvent& e) { if (menuOpen) { menuOpen = false; return; } juce::ComboBox::mouseUp (e); }

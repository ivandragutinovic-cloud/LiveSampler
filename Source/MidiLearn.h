#pragma once
#include <JuceHeader.h>

//==============================================================================
//  MIDI Learn: any knob or button can be assigned a CC (knobs and buttons)
//  or a key (buttons only). Right-click a control: MIDI Learn / Remove Learn.
//
//  Targets are registered by id (e.g. "perf.part3.volume"). A target either drives a value
//  (0..1 from the CC) or a toggle (on/off). Learned messages are taken out of the MIDI stream,
//  so a learned key does not play a note and a learned CC does not reach the sounds.
//==============================================================================
class MidiLearn : public juce::ChangeBroadcaster,
                  public juce::DeletedAtShutdown,
                  private juce::Timer
{
public:
    static MidiLearn& get();

    struct Target
    {
        bool isButton = false;
        std::function<void (float)> setNorm;     // knobs: 0..1
        std::function<bool()> getState;          // buttons
        std::function<void (bool)> setState;     // buttons
        std::function<void()> trigger;           // buttons without on/off state (one shot)
    };

    struct Assign
    {
        int type = 0;          // 1 = CC, 2 = note
        int channel = 1;       // 1..16
        int number = 0;
        juce::String describe() const;
    };

    // registration (message thread). A newer registration of the same id replaces the old one.
    void addTarget (const juce::String& id, Target t);
    void removeTarget (const juce::String& id);
    void removeAllTargets();
    void addKnob (const juce::String& id, double lo, double hi, double centre, std::function<void (double)> set);
    void addToggle (const juce::String& id, std::function<bool()> get, std::function<void (bool)> set);

    // helpers that drive an existing control
    void bindSlider (juce::Slider& s, const juce::String& id);
    void bindButton (juce::Button& b, const juce::String& id);
    void bindCombo (juce::ComboBox& c, const juce::String& id);

    // UI
    void showMenu (juce::Component* target, const juce::String& id, bool allowNote);
    void startLearn (const juce::String& id, bool allowNote);
    void cancelLearn();
    bool isLearning() const { return learnMode.load() != 0; }
    juce::String learningId() const { return learnTargetId; }
    bool isAssigned (const juce::String& id) const { return assigns.count (id) > 0; }
    juce::String describe (const juce::String& id) const;
    juce::StringArray idsFor (int type, int channel, int number) const;   // type 1 CC, 2 nota (message thread)
    bool stateOf (const juce::String& id, bool& state) const;             // false = nije dugme sa stanjem
    void clear (const juce::String& id);

    // audio thread: true = the message was used by MIDI Learn (do not pass it on)
    bool handleIncoming (const juce::MidiMessage& m);

    juce::File getFile() const;

private:
    MidiLearn();
    ~MidiLearn() override;
    static MidiLearn* instance;
    void timerCallback() override;
    void rebuildTable();
    void load();
    void save() const;
    void dispatch (int type, int channel, int number, int value);

    std::map<juce::String, Target> targets;
    std::map<juce::String, Assign> assigns;

    // 0 = none, 1 = learn CC only, 2 = learn CC or note
    std::atomic<int> learnMode { 0 };
    juce::String learnTargetId;
    bool learnAllowNote = false;

    // [type 0 = CC, 1 = note][channel][number]
    std::atomic<juce::uint8> table[2][16][128];

    struct Event { juce::uint8 type, channel, number, value; bool learn; };
    juce::AbstractFifo fifo { 1024 };
    Event events[1024];

    std::map<int, double> lastPress;
    std::map<int, double> lastDup;
    int blinkTick = 0;     // key -> ms of last "press" (momentary buttons)
};

//==============================================================================
//  Controls with right-click MIDI Learn. learnId returns "" when learn is not offered.
//==============================================================================
struct LearnMark
{
    static void paint (juce::Graphics& g, juce::Component& c, const juce::String& id);
};

class LearnSlider : public juce::Slider, private juce::ChangeListener
{
public:
    LearnSlider() { MidiLearn::get().addChangeListener (this); }
    ~LearnSlider() override { MidiLearn::get().removeChangeListener (this); }
    std::function<juce::String()> learnId;
    void setLearnId (const juce::String& id) { learnId = [id] { return id; }; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void paintOverChildren (juce::Graphics& g) override { if (learnId) LearnMark::paint (g, *this, learnId()); }
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    bool menuOpen = false;
};

class LearnButton : public juce::TextButton, private juce::ChangeListener
{
public:
    LearnButton() { MidiLearn::get().addChangeListener (this); }
    ~LearnButton() override { MidiLearn::get().removeChangeListener (this); }
    std::function<juce::String()> learnId;
    void setLearnId (const juce::String& id) { learnId = [id] { return id; }; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void paintOverChildren (juce::Graphics& g) override { if (learnId) LearnMark::paint (g, *this, learnId()); }
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    bool menuOpen = false;
};

class LearnCombo : public juce::ComboBox, private juce::ChangeListener
{
public:
    LearnCombo() { MidiLearn::get().addChangeListener (this); }
    ~LearnCombo() override { MidiLearn::get().removeChangeListener (this); }
    std::function<juce::String()> learnId;
    void setLearnId (const juce::String& id) { learnId = [id] { return id; }; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void paintOverChildren (juce::Graphics& g) override { if (learnId) LearnMark::paint (g, *this, learnId()); }
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    bool menuOpen = false;
};

#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class MappingEditor;
class PerformancePage;
class UserPage;
class TercePage;
class EffectsPage;
class SettingsPage;
class KronosPage;

namespace Pal
{
    const juce::Colour bg       { 0xff0f1115 };
    const juce::Colour panel    { 0xff1a1d24 };
    const juce::Colour panelHi  { 0xff262a33 };
    const juce::Colour text     { 0xffe8eaee };
    const juce::Colour dim      { 0xff8a91a0 };
    const juce::Colour lcdBg    { 0xff0b1a20 };
    const juce::Colour lcdText  { 0xff8fe9ff };
    const juce::Colour master   { 0xffc58aff };
    const juce::Colour accent   { 0xffff8a3d };
    const juce::Colour red      { 0xffff4d5e };
    const juce::Colour part[16] { juce::Colour (0xffff8a3d), juce::Colour (0xff3dc5ff), juce::Colour (0xff7ddf64), juce::Colour (0xffff5c8a),
                                  juce::Colour (0xffc58aff), juce::Colour (0xffffd23d), juce::Colour (0xff3dffd0), juce::Colour (0xffff9be0),
                                  juce::Colour (0xffd9a066), juce::Colour (0xff6f9dff), juce::Colour (0xffb8e05a), juce::Colour (0xffff7a6b),
                                  juce::Colour (0xff9be7ff), juce::Colour (0xffe8c9ff), juce::Colour (0xff5fd39b), juce::Colour (0xffffb36b) };
}

//==============================================================================
class SamplerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SamplerLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button& b, const juce::Colour& bg,
                               bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Font getPopupMenuFont() override { return juce::FontOptions (16.0f); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::FontOptions (14.0f); }
};

//==============================================================================
namespace UI
{
    juce::String noteName (int n, bool c3Is60);
    void setupIncDec (juce::Slider& s, double lo, double hi, double step,
                      std::function<juce::String (double)> fmt = {},
                      std::function<double (const juce::String&)> parse = {});
    void setupNoteBox (juce::Slider& s, std::function<bool()> c3Is60);
    void sectionTitle (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour c);
    juce::String dbText (double v);
    juce::String panText (double v);
    juce::String timeText (double v);
    juce::String hzText (double v);
    juce::String pctText (double v);

    // meni sa instrumentima, grupisano po folderima
    void showInstrumentMenu (juce::Component* target, const juce::StringArray& names, const juce::String& current,
                             bool allowEmpty, std::function<void (const juce::String&)> onPick);
}

// Potenciometar sa nazivom i vrednoscu (bez APVTS-a)
class ValueKnob : public juce::Component
{
public:
    ValueKnob (const juce::String& title, juce::Colour accent, double lo, double hi, double centre,
               std::function<juce::String (double)> fmt, double defaultValue);
    void setValue (double v) { if (! slider.isMouseButtonDown()) slider.setValue (v, juce::dontSendNotification); }
    double getValue() const { return slider.getValue(); }
    void setAccent (juce::Colour c) { slider.setColour (juce::Slider::rotarySliderFillColourId, c); }
    std::function<void (double)> onChange;
    void resized() override;

    juce::Slider slider;
    juce::Label label;
};

//==============================================================================
class LiveSamplerEditor : public juce::AudioProcessorEditor,
                          private juce::ChangeListener,
                          private juce::Timer
{
public:
    explicit LiveSamplerEditor (LiveSamplerProcessor&);
    ~LiveSamplerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int designW = 1280, designH = 860;

    enum Tab { TabPerformance = 0, TabUser, TabMapping, TabTerce, TabEffects, TabKronos, TabSettings, NumTabs };
    void showTab (int tab);
    void editUser (const juce::String& userName);            // otvori USER stranicu za ovog usera
    void editMapping (const juce::String& userName, int osc);
    void showMapping (bool show) { showTab (show ? TabMapping : TabPerformance); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshAll();
    void refreshSlots();

    struct Content : public juce::Component
    {
        LiveSamplerEditor* owner = nullptr;
        void paint (juce::Graphics&) override;
    } content;

    // gornja traka: displej performansa i terce
    struct Display : public juce::Component
    {
        LiveSamplerProcessor* proc = nullptr;
        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        std::function<void()> onClick;
    } display;

    struct TercaButton : public juce::Component, public juce::SettableTooltipClient
    {
        LiveSamplerProcessor* proc = nullptr;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override { proc->tercaButton (true); }
        void mouseUp (const juce::MouseEvent&) override   { proc->tercaButton (false); }
    } tercaButton;

    SamplerLookAndFeel lnf;
    LiveSamplerProcessor& proc;
    juce::Label logo, status;
    juce::TextButton panicButton, learnButton;
    juce::TextButton tabButtons[NumTabs];
    int currentTab = TabPerformance;

    std::unique_ptr<PerformancePage> performancePage;
    std::unique_ptr<UserPage> userPage;
    std::unique_ptr<MappingEditor> mappingEditor;
    std::unique_ptr<TercePage> tercePage;
    std::unique_ptr<EffectsPage> effectsPage;
    std::unique_ptr<SettingsPage> settingsPage;
    std::unique_ptr<KronosPage> kronosPage;
    std::vector<juce::Component*> pages() const;

    // performans mesta
    juce::TextButton bankButtons[kNumPerfBanks];
    juce::TextButton slotButtons[kPerfPerBank];
    juce::TextButton writeButton;

    juce::MidiKeyboardComponent keyboard;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveSamplerEditor)
};

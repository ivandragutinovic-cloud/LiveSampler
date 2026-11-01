#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

namespace Pal
{
    const juce::Colour bg       { 0xff121419 };
    const juce::Colour panel    { 0xff1c2028 };
    const juce::Colour panelHi  { 0xff262b35 };
    const juce::Colour text     { 0xffe8eaee };
    const juce::Colour dim      { 0xff8a91a0 };
    const juce::Colour part[3]  { juce::Colour (0xffff8a3d), juce::Colour (0xff3dc5ff), juce::Colour (0xff7ddf64) };
    const juce::Colour master   { 0xffc58aff };
}

//==============================================================================
class SamplerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SamplerLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& bg,
                               bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

//==============================================================================
// Potenciometar sa nazivom i vrednoscu, vezan za parametar
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
          const juce::String& title, juce::Colour accent);
    void resized() override;

private:
    juce::Slider slider;
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
class PartPanel : public juce::Component
{
public:
    PartPanel (LiveSamplerProcessor&, int partIndex);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    void showInstrumentMenu();
    void stepInstrument (int delta);

    LiveSamplerProcessor& proc;
    const int part;
    const juce::Colour accent;

    juce::TextButton onButton, monoButton, instrumentButton, prevButton { "<" }, nextButton { ">" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAtt, monoAtt;
    juce::ComboBox fxBox;
    juce::Label fxLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> fxAtt;
    juce::OwnedArray<Knob> knobs;    // redosled odgovara rasporedu u resized()
};

//==============================================================================
class MasterPanel : public juce::Component
{
public:
    explicit MasterPanel (LiveSamplerProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::TextButton splitButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> splitAtt;
    juce::OwnedArray<Knob> knobs;
    struct Section { juce::String title; int first, count; };
    juce::Array<Section> sections;
};

//==============================================================================
class RegisterBar : public juce::Component
{
public:
    explicit RegisterBar (LiveSamplerProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    void registerClicked (int i);

    LiveSamplerProcessor& proc;
    int bank = 0;
    juce::TextButton bankButtons[Registers::numBanks];
    juce::TextButton regButtons[Registers::perBank];
    juce::TextButton saveButton;
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

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshAll();

    SamplerLookAndFeel lnf;
    LiveSamplerProcessor& proc;

    struct Content : public juce::Component
    {
        void paint (juce::Graphics&) override;
    } content;

    juce::Label title, status;
    juce::TextButton folderButton, panicButton;
    juce::ToggleButton c3Toggle;
    std::unique_ptr<PartPanel> partPanels[LiveSamplerProcessor::numParts];
    MasterPanel masterPanel;
    RegisterBar registerBar;
    juce::MidiKeyboardComponent keyboard;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveSamplerEditor)
};

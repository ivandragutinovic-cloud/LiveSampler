#pragma once
#include "PluginEditor.h"

class LiveSamplerEditor;

//==============================================================================
//  PERFORMANS: 8 delova kao kanali miksete + pregled raspona tastera
//==============================================================================
class PartStrip : public juce::Component
{
public:
    PartStrip (LiveSamplerProcessor&, LiveSamplerEditor&, int index);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void edit (std::function<void (PerfPart&)> fn);
    void showUserMenu();

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    const int index;
    const juce::Colour colour;

    juce::TextButton onButton, userButton, editButton, tercaButton, sustainButton;
    ValueKnob volume, pan, reverb, delay;
    juce::Slider transpose, octave, keyLow, keyHigh, velLow, velHigh, channel;
    juce::Label labels[7];
};

class KeyRangeView : public juce::Component
{
public:
    explicit KeyRangeView (LiveSamplerProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
private:
    LiveSamplerProcessor& proc;
};

class PerformancePage : public juce::Component
{
public:
    PerformancePage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    LiveSamplerProcessor& proc;
    juce::Label nameLabel, nameEdit;
    juce::OwnedArray<PartStrip> strips;
    KeyRangeView ranges;
};

//==============================================================================
//  USER: 8 oscilatora + zajednicki parametri
//==============================================================================
class OscRow : public juce::Component
{
public:
    OscRow (LiveSamplerProcessor&, LiveSamplerEditor&, int osc, std::function<int()> userIndex);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void edit (std::function<void (OscSettings&)> fn);

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    const int osc;
    std::function<int()> userIndex;

    juce::TextButton onButton, instButton, mapButton;
    juce::Slider level, pan, transpose, fine, keyLow, keyHigh, velLow, velHigh;
};

class UserPage : public juce::Component,
                 private juce::ListBoxModel
{
public:
    UserPage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void selectUser (int index);
    int getUserIndex() const { return userIndex; }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void edit (std::function<void (UserProgram&)> fn);

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    int userIndex = 0;

    juce::ListBox list;
    juce::TextButton newButton, copyButton, deleteButton;
    juce::Label nameEdit, headerLabels[9];
    juce::OwnedArray<OscRow> rows;

    juce::OwnedArray<ValueKnob> knobs;   // attack, decay, sustain, release, cutoff, reso, velocity, porta, fx rate, depth, mix
    juce::TextButton monoButton;
    juce::Slider bendRange;
    juce::ComboBox fxType;
    juce::Label bendLabel, fxLabel;
};

//==============================================================================
//  TERCE
//==============================================================================
class TercePage : public juce::Component
{
public:
    TercePage (LiveSamplerProcessor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void edit (std::function<void (TercaSettings&)> fn);
    void rebuildRules();

    struct RuleRow : public juce::Component
    {
        RuleRow (TercePage& o, int i);
        void refresh();
        void resized() override;
        void paint (juce::Graphics&) override;
        TercePage& owner;
        const int index;
        juce::TextButton notes[12], deleteButton;
        juce::ComboBox scale;
        juce::Slider shift;
        juce::Label example;
    };

    LiveSamplerProcessor& proc;
    int scaleIndex = 0;

    juce::TextButton learnButton, onButton;
    juce::Label learnLabel, stateLabel;
    juce::ComboBox voiceMode, keyRoot, keyScale;
    ValueKnob velocity;
    juce::ToggleButton topOnly;

    juce::ComboBox scaleSelect;
    juce::TextButton addScale, deleteScale;
    juce::Label scaleName;
    juce::Label stepNames[12];
    juce::Slider above[12], below[12];
    juce::Label aboveLabel, belowLabel;

    juce::TextButton addRule;
    juce::Viewport rulesView;
    juce::Component rulesHolder;
    juce::OwnedArray<RuleRow> ruleRows;
    juce::Label rulesHelp;
};

//==============================================================================
//  EFEKTI (master efekti performansa)
//==============================================================================
class EffectsPage : public juce::Component
{
public:
    explicit EffectsPage (LiveSamplerProcessor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void edit (std::function<void (MasterSettings&)> fn);
    LiveSamplerProcessor& proc;
    juce::OwnedArray<ValueKnob> knobs;
    std::unique_ptr<ValueKnob> globalVolume;
    struct Section { juce::String title; int first, count; };
    juce::Array<Section> sections;
};

//==============================================================================
//  PODESAVANJA: folderi (disk, USB), C3
//==============================================================================
class SettingsPage : public juce::Component,
                     private juce::ListBoxModel
{
public:
    explicit SettingsPage (LiveSamplerProcessor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;

    LiveSamplerProcessor& proc;
    juce::ListBox folders;
    juce::TextButton addButton, removeButton, rescanButton;
    juce::ToggleButton c3Toggle;
    juce::Label info;
    std::unique_ptr<juce::FileChooser> chooser;
};

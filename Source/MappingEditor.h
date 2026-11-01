#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// Grafik: tasteri levo-desno, jacina udarca dole-gore. Zone se prevlace misem.
class ZoneMapView : public juce::Component,
                    private juce::Timer
{
public:
    explicit ZoneMapView (juce::MidiKeyboardState& keyboardState);

    void setZones (const std::vector<Zone>& z, int selectedIndex, bool c3Is60);

    std::function<void (int)> onSelect;
    std::function<void (const std::vector<Zone>&, bool finished)> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    static juce::Colour colourFor (int sampleIndex);

private:
    enum Mode { None, Move, Left, Right, Top, Bottom };

    void timerCallback() override;
    juce::Rectangle<float> plotArea() const;
    juce::Rectangle<float> zoneRect (const Zone&) const;
    Mode modeAt (int zoneIndex, juce::Point<float> p) const;
    int zoneAt (juce::Point<float> p) const;

    juce::MidiKeyboardState& keyboard;
    std::vector<Zone> zones;
    int selected = -1;
    bool c3 = true;
    std::array<bool, 128> held {};

    Mode mode = None;
    Zone dragStartZone;
    juce::Point<float> dragStart;
    bool dragged = false;
};

//==============================================================================
class MappingEditor : public juce::Component,
                      private juce::TableListBoxModel
{
public:
    explicit MappingEditor (LiveSamplerProcessor&);

    void setUserOsc (int userIndex, int oscIndex);
    void setOsc (int oscIndex) { setUserOsc (userIdx, oscIndex); }
    void refresh();                      // pozvati kad se procesor promeni
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    void commit (std::vector<Zone> newZones, bool pushUndo, bool coalesce = false);
    void select (int index);
    void updateInspector();
    void updateHeader();
    void undo();
    juce::String noteName (int n) const;
    void setupKeySlider (juce::Slider&);
    void setupValueSlider (juce::Slider&, double lo, double hi, double step, const juce::String& suffix);
    void editSelected (std::function<void (Zone&)> fn, bool coalesce = true);

    LiveSamplerProcessor& proc;
    int userIdx = 0, osc = 0;
    juce::String instName;
    InstrumentPtr inst;
    std::vector<Zone> zones;
    int selected = -1;
    std::set<juce::String> dirtyNames;
    std::vector<std::vector<Zone>> undoStack;
    juce::uint32 lastUndoPush = 0;
    bool updatingUI = false;

    juce::Label title, instLabel, stateLabel, sampleInfo, hint;
    juce::TextButton oscButtons[kNumOsc];
    juce::Label userLabel;
    juce::TextButton undoButton, saveButton, resetButton, revertButton, closeButton;

    ZoneMapView mapView;
    juce::TableListBox table;

    juce::Label sliderLabels[8];
    juce::Slider rootSlider, lowKeySlider, highKeySlider, lowVelSlider, highVelSlider,
                 tuneSlider, gainSlider, panSlider;
    juce::ToggleButton enabledToggle, loopToggle, fixedToggle;
    juce::TextButton duplicateButton, deleteButton, octDownButton, octUpButton, rangesButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MappingEditor)
};

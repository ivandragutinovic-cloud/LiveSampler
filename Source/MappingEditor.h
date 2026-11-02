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
// Editor sempla i loop-a za izabranu zonu (kao Sample Edit / Loop Edit na Korgu):
// Start, Loop Start i End se vuku misem ili upisuju; LOOP prikaz pokazuje spoj kraja i pocetka loop-a.
class WaveEditor : public juce::Component
{
public:
    WaveEditor();

    struct Points { int start = 0, loopStart = 0, end = 0; bool loop = false; };
    void setSample (SamplePtr s, const Points& p);
    void clear();

    // finished = pusten mis / upisana vrednost (za undo)
    std::function<void (const Points&, bool finished)> onChange;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum Marker { None = -1, Start = 0, LoopStart, End };
    juce::Rectangle<int> waveArea() const;
    double xToSample (float x) const;
    float sampleToX (double s) const;
    Marker markerAt (float x) const;
    int snap (int pos) const;
    void apply (Points np, bool finished);
    void updateBoxes();
    void zoomToFit();

    SamplePtr sample;
    Points pts;
    bool loopView = false;
    double viewStart = 0.0, viewLen = 1.0;      // prikaz u semplovima
    int seamHalf = 400;                         // LOOP prikaz: koliko semplova sa svake strane spoja
    Marker drag = None;
    bool panning = false;
    double panStartView = 0.0;
    float panStartX = 0.0f;

    juce::TextButton sampleMode, loopMode, fitButton;
    juce::ToggleButton loopToggle, zeroToggle;
    juce::Label labels[3];
    juce::Slider boxes[3];
    bool updating = false;
};

//==============================================================================
class MappingEditor : public juce::Component,
                      private juce::TableListBoxModel
{
public:
    explicit MappingEditor (LiveSamplerProcessor&);

    void setUserOsc (int userIndex, int oscIndex);
    void setOsc (int oscIndex) { setUserOsc (userIdx, oscIndex); }
    int getUserIndex() const { return userIdx; }
    int getOsc() const { return osc; }
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
    juce::TextButton blockSel[kNumBlocks], msSel[kMsPerOsc];   // izbor OSC-a i MS-a
    juce::Label userLabel;
    juce::ComboBox userBox;          // koji user se mapira
    juce::StringArray shownUsers;
    juce::TextButton undoButton, saveButton, resetButton, revertButton, closeButton;

    ZoneMapView mapView;
    WaveEditor waveEditor;
    juce::TextButton viewButtons[2];      // MAPA / SEMPL I LOOP
    bool waveView = false;
    void setWaveView (bool w);
    void updateWave();
    juce::TableListBox table;

    juce::Label sliderLabels[8];
    juce::Slider rootSlider, lowKeySlider, highKeySlider, lowVelSlider, highVelSlider,
                 tuneSlider, gainSlider, panSlider;
    juce::ToggleButton enabledToggle, loopToggle, fixedToggle;
    juce::TextButton duplicateButton, deleteButton, octDownButton, octUpButton, rangesButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MappingEditor)
};

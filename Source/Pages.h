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
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    void edit (std::function<void (PerfPart&)> fn);
    void showUserMenu();

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    const int index;
    const juce::Colour colour;

    void showEq();
    void showPartMenu();
    LearnButton onButton, partMenu, userButton, editButton, eqButton, tercaButton, sustainButton, exprButton, userPrev, userNext;
    void stepUser (int dir);
    ValueKnob volume, pan, reverb, delay;
    LearnSlider transpose, octave, keyLow, keyHigh, velLow, velHigh, channel;
    juce::Label labels[7];
};

class KeyRangeView : public juce::Component
{
public:
    explicit KeyRangeView (LiveSamplerProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    // zone se pomeraju misem ili prstom: ivica = donja/gornja granica, sredina = cela zona
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
private:
    struct Hit { int part = -1, mode = 0; };   // mode: 1 donja ivica, 2 gornja ivica, 3 cela zona
    Hit hitTest (juce::Point<float>) const;
    int keyAt (float x) const;
    juce::Rectangle<float> barArea() const;
    juce::Array<int> activeParts() const;
    LiveSamplerProcessor& proc;
    Hit drag;
    int dragStartKey = 0, origLo = 0, origHi = 127;
};

//==============================================================================
//  SCENE: 8 dugmadi trenutnog performansa (klik = pozovi, STORE + klik = sacuvaj)
//==============================================================================
class SceneBar : public juce::Component
{
public:
    SceneBar (LiveSamplerProcessor&, bool big);
    void refresh();
    void resized() override;
private:
    void clicked (int s);
    void menu();
    LiveSamplerProcessor& proc;
    const bool big;
    LearnButton scenes[kNumScenes], storeButton, moreButton;
    juce::Label title;
};

//==============================================================================
//  STIMOVANJE (orijentalno / cetvrttonovi): ON, makam + tonika, QT EDIT (tasteri), 12 tonova u centima
//==============================================================================
class TuningBar : public juce::Component
{
public:
    explicit TuningBar (LiveSamplerProcessor&);
    void refresh();
    void resized() override;
private:
    LiveSamplerProcessor& proc;
    LearnButton onButton, qtButton;
    juce::TextButton presetButton, cells[12];
    juce::Label title;
};

//==============================================================================
//  LIVE: ekran za svirku (veliko ime, scene, jacine delova, transpose, harmony, LOCK)
//==============================================================================
class LivePage : public juce::Component
{
public:
    LivePage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;
private:
    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    SceneBar scenes { proc, true };
    TuningBar tuning { proc };
    LearnButton prevButton, nextButton, transDown, transUp, transZero, harmButton, panicButton, lockButton;
    struct Fader
    {
        LearnSlider volume;
        LearnButton on;
        juce::Label name;
    };
    Fader faders[kNumParts];
    juce::Rectangle<int> lcd, transArea;
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
    LiveSamplerEditor& editor;
    juce::Label nameLabel, nameEdit;
    juce::OwnedArray<PartStrip> strips;
    KeyRangeView ranges;
    SceneBar sceneBar { proc, false };
    LearnButton pageButtons[2];
    int page = 0;   // 0 = delovi 1-8, 1 = delovi 9-16
    // vector joystick (kao na Kronosu): ukljuci, equal power, XY polje, CC za X i Y (LEARN)
    std::unique_ptr<juce::Component> vectorPad;
    LearnButton vectorOn, vectorEqual, vectorCcX, vectorCcY, saveButton;
    void updateVectorButtons();
    int learnAxis = -1;
};

//==============================================================================
//  FM editor (user sa FM sintezom umesto MS1-MS8): algoritam, 6 operatora, envelope
//==============================================================================
class FmEditor : public juce::Component
{
public:
    FmEditor (LiveSamplerProcessor&, std::function<int()> userIndex);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    Fm::Patch current() const;
    void edit (std::function<void (Fm::Patch&)> fn);
    void selectEg (int which);              // 0-5 = EG operatora (DX OP1-6), 6 = Amp EG

    LiveSamplerProcessor& proc;
    std::function<int()> userIndex;
    bool updating = false;
    int egSel = 0;

    juce::Label algLabel, egTitle;
    LearnCombo algorithm;
    LearnSlider transpose;
    std::unique_ptr<ValueKnob> level, velocity;
    struct OpRow
    {
        LearnButton on, egButton;
        LearnCombo wave;
        LearnSlider coarse, fine, fixedHz, volume, vel, feedback;
    };
    OpRow ops[6];
    juce::Label colLabels[8];
    LearnButton ampEgButton;
    juce::OwnedArray<ValueKnob> egKnobs;
    juce::Rectangle<int> diagram;
};

//==============================================================================
//  USER: 8 oscilatora + zajednicki parametri
//==============================================================================
class OscRow : public juce::Component
{
public:
    // row = MS1..MS8; blockFn = trenutno izabran OSC1 (0) ili OSC2 (1)
    OscRow (LiveSamplerProcessor&, LiveSamplerEditor&, int row, std::function<int()> userIndex, std::function<int()> blockFn);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void edit (std::function<void (OscSettings&)> fn);

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    const int row;
    std::function<int()> userIndex, blockFn;
    int oscIndex() const { return blockFn() * kMsPerOsc + row; }

    void showMenu();
    void showEq();
    void showKeyFollow();
    LearnButton onButton, menuButton, instButton, mapButton, keyOffButton, eqButton, instPrev, instNext;
    void stepInstrument (int dir);
    void pickInstrument();
    LearnSlider level, pan, transpose, fine, keyLow, keyHigh, velLow, velHigh;
};

// Prikaz envelope (Kronos stil)
class EnvelopeView : public juce::Component
{
public:
    void setEnvelope (const EnvSettings& e, bool bipolar, juce::Colour c) { env = e; bip = bipolar; colour = c; repaint(); }
    void paint (juce::Graphics&) override;
private:
    EnvSettings env;
    bool bip = false;
    juce::Colour colour { Pal::accent };
};

// Editor zvuka: Zvuk A/B (Pitch, Filter, Amp) + Zajednicko + Efekat
class SoundEditor : public juce::Component
{
public:
    SoundEditor (LiveSamplerProcessor&, std::function<int()> userIndex);
    void refresh();
    int getBlock() const { return block; }
    std::function<void()> onBlockChanged;
    void resized() override;
    void paint (juce::Graphics&) override;

    enum Section { Pitch = 0, Filter, Amp, MonoArtic, Common, Effect, NumSections };

    struct Param
    {
        juce::String name;
        double lo, hi, centre, def;
        std::function<juce::String (double)> fmt;
        std::function<float& (UserProgram&, int block)> ref;
    };

private:
    void refreshLegatoKnobs();
    void showSection (int section);
    void rebuildKnobs();
    void edit (std::function<void (UserProgram&)> fn);
    const EnvSettings* currentEnv (const UserProgram& u, bool& bipolar) const;

    LiveSamplerProcessor& proc;
    std::function<int()> userIndex;
    int section = Filter, block = 0;

    LearnButton blockButtons[kNumBlocks], blockMenu, sectionButtons[NumSections], oscOnButton;
    void showBlockMenu();
    // mono opseg i prelaz (za izabrani OSC)
    juce::Label monoLabel, articLabel, articHelp;
    LearnSlider monoLow, monoHigh;
    LearnButton articOn, intervalButtons[24];
    LearnCombo articDir;
    std::vector<std::vector<Param>> rows;
    juce::OwnedArray<ValueKnob> knobs;
    EnvelopeView envView;

    // filter
    LearnCombo filterType;
    juce::Label filterLabel;
    // zajednicko
    LearnCombo voiceMode, priority;
    LearnButton portaOn, portaFingered, portaConstRate;
    std::unique_ptr<ValueKnob> portaTime, legatoXfade, legatoStart;
    LearnSlider bendUp, bendDown;
    juce::Label commonLabels[4];
    // efekat
    LearnCombo fxType;
    juce::Label fxLabel;
};

class UserPage : public juce::Component,
                 private juce::ListBoxModel
{
public:
    UserPage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void selectUser (int index);
    int getUserIndex() const { return userIndex; }
    int getSoundBlock() const { return sound.getBlock(); }
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
    LearnButton newButton, copyButton, deleteButton;
    // lista usera: search, kategorija, redosled (broj), A-Z
    juce::TextEditor search;
    LearnCombo catFilter;
    LearnButton sortButton, catButton, numButton, upButton, downButton;
    std::vector<int> visible;           // red u listi -> index usera
    bool sortAlpha = false;
    void rebuildVisible();
    void categoryMenu();
    void askNumber();
    juce::Label nameEdit, headerLabels[10];
    juce::OwnedArray<OscRow> rows;
    FmEditor fm;
    SoundEditor sound;
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
        LearnButton notes[12], deleteButton;
        LearnCombo scale;
        LearnSlider shift;
        juce::Label example;
    };

    LiveSamplerProcessor& proc;
    int scaleIndex = 0;

    LearnButton learnButton, onButton;
    juce::Label learnLabel, stateLabel;
    LearnCombo voiceMode, keyRoot, keyScale;
    ValueKnob velocity;
    juce::ToggleButton topOnly;

    LearnCombo scaleSelect;
    LearnButton addScale, deleteScale;
    juce::Label scaleName;
    juce::Label stepNames[12];
    LearnSlider above[12], below[12];
    juce::Label aboveLabel, belowLabel;

    LearnButton addRule;
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
                     private juce::ListBoxModel,
                     private juce::Timer
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
    LearnButton addButton, removeButton, rescanButton, backupButton, restoreButton;
    juce::ToggleButton c3Toggle;
    juce::Label info, backupStatus, velCurveLabel;
    LearnCombo velCurveBox;
    void timerCallback() override;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
//  KRONOS: uvoz programa iz PCG fajla
//==============================================================================
class KronosPage : public juce::Component,
                   private juce::TableListBoxModel
{
public:
    KronosPage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void cellDoubleClicked (int row, int col, const juce::MouseEvent&) override;
    void playRow (int row);
    void loadCombiRow (int row);
    void loadCombisToList (const juce::SparseSet<int>& rows);   // vise kombija odjednom -> mesta u listi
    void offerAutosample (const juce::String& message, const juce::String& args);
    void offerMissing (const juce::String& message, const std::vector<LiveSamplerProcessor::KronosMissing>& missing, const juce::String& pcgPath);
public:
    static juce::File findAutosampler();
    static void startAutosampler (const juce::String& args);
private:
    void setMode (bool combis);
    juce::String timbreSummary (const Kronos::Combi& c, int& romCount) const;
    void rebuildRows();
    juce::String msText (const Kronos::Zone& z) const;
    int userFor (const Kronos::Program& p);
    void loadPcg (const juce::File& f);

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    const Kronos::Pcg* shownPcg = nullptr;
    std::vector<int> rows;
    std::unique_ptr<juce::FileChooser> chooser;

    LearnButton loadButton, createSelected, createAll, playButton, modeButtons[2];
    bool combiMode = false;
    juce::ToggleButton onlyUser, keepChannels, fmToggle;
    juce::TextEditor search;
    juce::Label fileLabel, infoLabel, help;
    juce::TableListBox table;
};

//==============================================================================
//  LISTE: 16 lista po 128 performansa (kao Kronos banke kombija)
//==============================================================================
class ListsPage : public juce::Component, public juce::DragAndDropTarget
{
public:
    ListsPage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    // prevlacenje performansa: na drugo mesto (zamena), na dugme liste (druga lista), dole na set listu
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("perf:"); }
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override { dropCell = -1; repaint(); }
    void itemDropped (const SourceDetails&) override;

private:
    int pressCell = -1, dropCell = -1;
    bool dragStarted = false;
    juce::Rectangle<float> cellRect (int i) const;
    juce::Rectangle<int> gridArea() const;
    int cellAt (juce::Point<int> p) const;
    void cellMenu (int index);
    void editCategories();
    void categoriesMenu();
public:
    void armWrite();            // SNIMI ukljucen: sledeci klik na mesto cuva trenutni performans
private:
    std::unique_ptr<juce::FileChooser> chooser;
    bool passesFilter (const Performance& s) const;

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    LearnButton bankButtons[kNumPerfBanks], writeButton, categoriesButton;
    juce::Label nameLabel, nameEdit, help, filterLabel, missingLabel;
    LearnCombo catFilter;
};

//==============================================================================
//  SET LISTA: velika dugmad za svirku (kao Montage / Kronos Set List)
//==============================================================================
class SetListPage : public juce::Component
{
public:
    SetListPage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static juce::Colour slotColour (int c);

private:
    juce::Rectangle<int> gridArea() const;
    int cellAt (juce::Point<int> p) const;
    void editSlot (int slot);

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    LearnCombo listSelect;
    juce::Label nameEdit;
    LearnButton pageButtons[kSetListSlots / 16], editButton;
    juce::ToggleButton pcMode;
};

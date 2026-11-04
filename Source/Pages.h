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
    LiveSamplerEditor& editor;
    juce::Label nameLabel, nameEdit;
    juce::OwnedArray<PartStrip> strips;
    KeyRangeView ranges;
    LearnButton pageButtons[2];
    int page = 0;   // 0 = delovi 1-8, 1 = delovi 9-16
    // vector joystick (kao na Kronosu): ukljuci, equal power, XY polje, CC za X i Y (LEARN)
    std::unique_ptr<juce::Component> vectorPad;
    LearnButton vectorOn, vectorEqual, vectorCcX, vectorCcY, saveButton;
    void updateVectorButtons();
    int learnAxis = -1;
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
    std::unique_ptr<ValueKnob> portaTime;
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
    static juce::File findAutosampler();
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
    juce::ToggleButton onlyUser, keepChannels;
    juce::TextEditor search;
    juce::Label fileLabel, infoLabel, help;
    juce::TableListBox table;
};

//==============================================================================
//  LISTE: 16 lista po 128 performansa (kao Kronos banke kombija)
//==============================================================================
class ListsPage : public juce::Component
{
public:
    ListsPage (LiveSamplerProcessor&, LiveSamplerEditor&);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
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

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
    juce::TextButton onButton, partMenu, userButton, editButton, eqButton, tercaButton, sustainButton;
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
    juce::TextButton pageButtons[2];
    int page = 0;   // 0 = delovi 1-8, 1 = delovi 9-16
    // vector joystick (kao na Kronosu): ukljuci, equal power, XY polje, CC za X i Y (LEARN)
    std::unique_ptr<juce::Component> vectorPad;
    juce::TextButton vectorOn, vectorEqual, vectorCcX, vectorCcY;
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
    juce::TextButton onButton, menuButton, instButton, mapButton, keyOffButton, eqButton;
    juce::Slider level, pan, transpose, fine, keyLow, keyHigh, velLow, velHigh;
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

    juce::TextButton blockButtons[kNumBlocks], blockMenu, sectionButtons[NumSections];
    void showBlockMenu();
    // mono opseg i prelaz (za izabrani OSC)
    juce::Label monoLabel, articLabel, articHelp;
    juce::Slider monoLow, monoHigh;
    juce::TextButton articOn, intervalButtons[24];
    juce::ComboBox articDir;
    std::vector<std::vector<Param>> rows;
    juce::OwnedArray<ValueKnob> knobs;
    EnvelopeView envView;

    // filter
    juce::ComboBox filterType;
    juce::Label filterLabel;
    // zajednicko
    juce::ComboBox voiceMode, priority;
    juce::TextButton portaOn, portaFingered, portaConstRate;
    std::unique_ptr<ValueKnob> portaTime;
    juce::Slider bendUp, bendDown;
    juce::Label commonLabels[4];
    // efekat
    juce::ComboBox fxType;
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
    juce::TextButton newButton, copyButton, deleteButton;
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

    juce::TextButton loadButton, createSelected, createAll, playButton, modeButtons[2];
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
    std::unique_ptr<juce::FileChooser> chooser;
    bool passesFilter (const Performance& s) const;

    LiveSamplerProcessor& proc;
    LiveSamplerEditor& editor;
    juce::TextButton bankButtons[kNumPerfBanks], writeButton, categoriesButton;
    juce::Label nameLabel, nameEdit, help, filterLabel, missingLabel;
    juce::ComboBox catFilter;
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
    juce::ComboBox listSelect;
    juce::Label nameEdit;
    juce::TextButton pageButtons[kSetListSlots / 16], editButton;
    juce::ToggleButton pcMode;
};

#pragma once
#include "PluginEditor.h"

class LiveSamplerEditor;

//==============================================================================
//  PERFORMANS: 8 delova kao kanali miksete + pregled raspona tastera
//==============================================================================
// VU metar (vrh, dBFS): zeleno do -12, zuto do -3, crveno iznad; linija = najveci vrh zadnje 1.5 s; CLIP svetli 2 s
class VuMeter : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit VuMeter (std::function<float (int ch)> takePeak, bool stereo = true, bool vertical = true)
        : take (std::move (takePeak)), channels (stereo ? 2 : 1), vert (vertical)
    {
        setInterceptsMouseClicks (true, false);
        startTimerHz (30);
    }
    void mouseDown (const juce::MouseEvent&) override { for (auto& h : hold) h = -60.0f; clip = 0; repaint(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0d0f12));
        g.fillRoundedRectangle (r, 2.0f);
        r = r.reduced (1.0f);
        const float lo = -48.0f, hi = 3.0f;
        auto pos = [lo, hi] (float db) { return juce::jlimit (0.0f, 1.0f, (db - lo) / (hi - lo)); };
        for (int ch = 0; ch < channels; ++ch)
        {
            auto lane = vert ? r.withWidth (r.getWidth() / (float) channels).translated (r.getWidth() / (float) channels * (float) ch, 0.0f).reduced (0.5f, 0.0f)
                             : r.withHeight (r.getHeight() / (float) channels).translated (0.0f, r.getHeight() / (float) channels * (float) ch).reduced (0.0f, 0.5f);
            const float f = pos (level[ch]);
            juce::ColourGradient grad = vert ? juce::ColourGradient (juce::Colour (0xffe53935), lane.getX(), lane.getY(), juce::Colour (0xff2fbf5a), lane.getX(), lane.getBottom(), false)
                                             : juce::ColourGradient (juce::Colour (0xff2fbf5a), lane.getX(), lane.getY(), juce::Colour (0xffe53935), lane.getRight(), lane.getY(), false);
            grad.addColour (pos (-12.0f), juce::Colour (0xff2fbf5a));
            grad.addColour (pos (-3.0f), juce::Colour (0xffe0c020));
            g.setGradientFill (grad);
            if (vert) g.fillRect (lane.withTop (lane.getBottom() - lane.getHeight() * f));
            else g.fillRect (lane.withWidth (lane.getWidth() * f));
            const float hp = pos (hold[ch]);
            g.setColour (hold[ch] > -3.0f ? juce::Colour (0xffff6060) : juce::Colours::white.withAlpha (0.8f));
            if (hold[ch] > lo)
            {
                if (vert) g.fillRect (lane.getX(), lane.getBottom() - lane.getHeight() * hp - 1.0f, lane.getWidth(), 2.0f);
                else g.fillRect (lane.getX() + lane.getWidth() * hp - 1.0f, lane.getY(), 2.0f, lane.getHeight());
            }
        }
        // skala: 0 dB i -12 dB
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        for (float db : { 0.0f, -12.0f, -24.0f })
        {
            const float p = pos (db);
            if (vert) g.drawHorizontalLine ((int) (r.getBottom() - r.getHeight() * p), r.getX(), r.getRight());
            else g.drawVerticalLine ((int) (r.getX() + r.getWidth() * p), r.getY(), r.getBottom());
        }
        if (clip > 0)
        {
            g.setColour (juce::Colour (0xffff2020));
            if (vert) g.fillRect (r.withHeight (4.0f)); else g.fillRect (r.withLeft (r.getRight() - 4.0f));
        }
    }
private:
    void timerCallback() override
    {
        if (! isShowing()) return;
        const auto now = juce::Time::getMillisecondCounter();
        for (int ch = 0; ch < channels; ++ch)
        {
            float pk = take (ch);
            if (channels == 1) pk = juce::jmax (pk, take (1));
            const float db = juce::Decibels::gainToDecibels (pk, -60.0f);
            level[ch] = db >= level[ch] ? db : juce::jmax (db, level[ch] - 24.0f / 30.0f);   // pada 24 dB/s
            if (db >= hold[ch]) { hold[ch] = db; holdTime[ch] = now; }
            else if (now - holdTime[ch] > 1500) hold[ch] = juce::jmax (-60.0f, hold[ch] - 1.0f);
            if (pk >= 0.999f) clip = 60;
        }
        if (clip > 0) --clip;
        repaint();
    }
    std::function<float (int)> take;
    const int channels;
    const bool vert;
    float level[2] { -60.0f, -60.0f }, hold[2] { -60.0f, -60.0f };
    juce::uint32 holdTime[2] {};
    int clip = 0;
};

// tackica: svetli kad part svira (jako na udarac, slabije dok zvuk traje)
class ActivityDot : public juce::Component, private juce::Timer
{
public:
    ActivityDot (LiveSamplerProcessor& p, int part, juce::Colour c) : proc (p), index (part), colour (c)
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override
    {
        const float d = juce::jmin ((float) getWidth(), (float) getHeight()) - 2.0f;
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre (d, d);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (r);
        const float a = juce::jmax (flash, sounding ? 0.35f : 0.0f);
        if (a > 0.0f)
        {
            g.setColour (colour.brighter (0.4f).withAlpha (a));
            g.fillEllipse (r.reduced (1.0f));
            if (flash > 0.5f) { g.setColour (colour.withAlpha (0.35f * flash)); g.drawEllipse (r.expanded (0.5f), 1.5f); }
        }
    }
private:
    void timerCallback() override
    {
        const auto h = proc.getPartHits (index);
        const bool snd = proc.getPartVoices (index) > 0;
        const float f = h != lastHits ? 1.0f : juce::jmax (0.0f, flash - 0.12f);
        lastHits = h;
        if (f != flash || snd != sounding) { flash = f; sounding = snd; repaint(); }
    }
    LiveSamplerProcessor& proc;
    const int index;
    juce::Colour colour;
    juce::uint32 lastHits = 0;
    float flash = 0.0f;
    bool sounding = false;
};

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
    void showHarmLevel();
    void showPartMenu();
    std::unique_ptr<ActivityDot> dot;
    std::unique_ptr<VuMeter> meter;
    LearnButton onButton, soloButton, partMenu, userButton, editButton, eqButton, tercaButton, sustainButton, exprButton, userPrev, userNext;
    void stepUser (int dir);
    ValueKnob volume, pan, reverb, delay;
    LearnSlider transpose, octave, tune, keyLow, keyHigh, velLow, velHigh, channel;
    juce::Label labels[8];
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
    LearnButton prevButton, nextButton, transDown, transUp, transZero, harmButton, panicButton, lockButton, gigButton;
    juce::Label cpuLabel;
public:
    void updateMeter();
private:
    struct Fader
    {
        LearnSlider volume;
        LearnButton on;
        juce::Label name;
        std::unique_ptr<ActivityDot> dot;
        std::unique_ptr<VuMeter> meter;
    };
    std::unique_ptr<VuMeter> masterMeter;
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
    LearnButton pageButtons[2], keysButton, activeButton;
    int page = 0;   // 0 = delovi 1-8, 1 = delovi 9-16 (u ACTIVE prikazu: prvih / drugih 8 ukljucenih)
    std::vector<int> shownParts() const;   // koji partovi se vide (svi, ili samo ukljuceni)
    std::vector<int> lastShown;
    // vector joystick (kao na Kronosu): ukljuci, equal power, XY polje, CC za X i Y (LEARN)
    std::unique_ptr<juce::Component> vectorPad;
    LearnButton vectorOn, vectorEqual, vectorCcX, vectorCcY, saveButton;
    void updateVectorButtons();
    int learnAxis = -1;
};

//==============================================================================
//  FM editor (user sa FM sintezom umesto MS1-MS8): algoritam, 6 operatora, envelope
//==============================================================================
// VST3 plugin user: ime plugina i dugme za njegov prozor (zvuk se podesava u pluginu)
class PluginUserPanel : public juce::Component
{
public:
    PluginUserPanel (LiveSamplerProcessor&, std::function<int()> userIndex);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;
    static void openWindow (LiveSamplerProcessor&, int userIndex);
private:
    LiveSamplerProcessor& proc;
    std::function<int()> userIndex;
    juce::Label title, info;
    juce::TextButton openButton;
};

class VlEditor : public juce::Component
{
public:
    VlEditor (LiveSamplerProcessor&, std::function<int()> userIndex);
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    Vl::Patch current() const;
    void edit (std::function<void (Vl::Patch&)> fn);
    LiveSamplerProcessor& proc;
    std::function<int()> userIndex;
    bool updating = false;
    juce::Label modelLabel, sourceLabel, transLabel, hint;
    LearnCombo model, source;
    LearnSlider transpose;
    juce::OwnedArray<ValueKnob> knobs;
};

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
    void showXfade();
    void showKeyFollow();
    LearnButton onButton, menuButton, instButton, mapButton, keyOffButton, eqButton, xfButton, instPrev, instNext;
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
    LearnButton portaOn, portaFingered, portaConstRate, levelLockButton, autoLevelButton;
    std::unique_ptr<VuMeter> commonMeter;
    std::unique_ptr<ValueKnob> portaTime, legatoXfade, legatoStart, userLevel;
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
    // SOLO USER: user se presluskuje sam (bez performansa). Klik na usera u listi pali, EDIT iz performansa gasi.
    void setAudition (bool on);
    void setPageActive (bool active) { pageActive = active; applyAudition(); }

private:
    bool audition = false, pageActive = false;
    void applyAudition();
    LearnButton auditionButton, originalButton, autoAllButton;
    juce::Label autoLevelInfo;
    void showAutoLevelMenu();
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
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
    VlEditor vl;
    PluginUserPanel plug;
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
    std::unique_ptr<VuMeter> masterMeter;
    juce::OwnedArray<ValueKnob> geq;          // MASTER EQ (ceo Workstation): parametric
    juce::StringArray geqKeys;
    juce::OwnedArray<juce::Slider> gband;     // graphic 15
    juce::TextButton geqOn, geqParam, geqGraphic, geqFlat;
    juce::ComboBox revType;
    juce::TextButton revPreset;
    void showReverbPresets();
    void editEq (std::function<void (GlobalEqSettings&)> fn);
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
    LearnButton addButton, removeButton, rescanButton, backupButton, restoreButton, cleanupButton;
    juce::ToggleButton c3Toggle, gigToggle, streamToggle, guardToggle;
    juce::TextButton guardLogButton;
    juce::Label info, backupStatus, velCurveLabel, polyLabel, audioLabel;
    LearnCombo velCurveBox;
    juce::ComboBox polyBox;
    juce::TextButton resetDrops;
    void timerCallback() override;
    std::unique_ptr<juce::FileChooser> chooser;
    // MREZA miniPC-a: KUCA.bat / SVIRKA.bat preko zadatka u Task Scheduler-u (admin), dugme ili MIDI (dva pritiska)
    LearnButton netHome, netGig, netSetup, powerOff;
    juce::Label netLabel;
    void netPress (int which, bool fromMidi);
    void shutdownPc (bool fromMidi);
    void netSetupMenu();
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
class ListsPage : public juce::Component, public juce::DragAndDropTarget, public juce::TooltipClient
{
public:
    ListsPage (LiveSamplerProcessor&, LiveSamplerEditor&);
    juce::String getTooltip() override;   // puno ime performansa ispod misa
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
    std::vector<std::unique_ptr<juce::MouseListener>> bankRight;   // desni klik na dugme liste
    void bankCategoryMenu (int bank);
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

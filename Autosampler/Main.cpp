// Pikaso Autosampler (Kronos): presempluje fabricke ROM / EXs multisemplove koje koriste izabrani programi i kombiji.
// 1) iz PCG-a nadje koje ROM multisemplove trebaju
// 2) napravi PCG sa po jednim programom za svaki multisempl (OSC1 MS1 = taj multisempl, bez efekata)
// 3) preko MIDI-ja bira te programe na Kronosu, svira svaki treci ton i snima preko USB audio ulaza
// 4) cuva WAV fajlove + kronos-ms.txt, pa ih Pikaso Workstation koristi umesto ROM zvuka.
// EXi programi (AL-1, CX-3, STR-1, MS-20EX, PolysixEX, MOD-7, SGX-2, EP-1) nemaju multisemplove:
// ceo program se salje u edit buffer (SysEx), bez efekata, i snima se svaki treci ton u vise jacina.
#include <JuceHeader.h>
#include "KronosPcg.h"
#include "KronosNames.h"
#include "AutoLoop.h"
#include <optional>

namespace
{
    const juce::Colour bg (0xff0f1115), panel (0xff1a1d24), accent (0xffff8a3d), dim (0xff8a93a6), lcd (0xff8fe9ff);

    juce::String noteName (int n) { return juce::MidiMessage::getMidiNoteName (n, true, true, 3); }

    // sirovi pristup programima u PCG-u (za citanje sablona i upis novih programa)
    struct RawBank { juce::uint32 bankId = 0; size_t firstItem = 0; int count = 0, itemSize = 0; bool exi = false; };

    std::vector<RawBank> findProgramBanks (const juce::MemoryBlock& mb)
    {
        std::vector<RawBank> out;
        auto* d = (const juce::uint8*) mb.getData();
        auto be32 = [d] (size_t i) { return ((juce::uint32) d[i] << 24) | ((juce::uint32) d[i + 1] << 16) | ((juce::uint32) d[i + 2] << 8) | d[i + 3]; };
        std::function<void (size_t, size_t)> walk = [&] (size_t s, size_t e)
        {
            size_t i = s;
            while (i + 12 <= e)
            {
                const juce::String id (juce::CharPointer_ASCII ((const char*) d + i), 4);
                const size_t size = be32 (i + 4), content = i + 12;
                if (content + size > e || size == 0) break;
                if (id == "PCG1" || id == "PRG1") walk (content, content + size);
                else if ((id == "PBK1" || id == "MBK1") && size >= 12)
                {
                    RawBank b;
                    b.count = (int) be32 (content);
                    b.itemSize = (int) be32 (content + 4);
                    b.bankId = be32 (content + 8);
                    b.firstItem = content + 12;
                    b.exi = id == "MBK1";
                    if (b.itemSize == Kronos::programSize) out.push_back (b);
                }
                i = content + size;
            }
        };
        walk (16, mb.getSize());
        return out;
    }

    struct NeededMs
    {
        juce::String romKey;            // "rom:" + 20 bajtova zone
        std::array<juce::uint8, 20> zone {};
        juce::String usedBy;
        int slot = -1;                  // broj programa u napravljenoj banci
        // EXi program: snima se ceo program
        bool exi = false;
        juce::String engine;            // npr. "AL-1" ili "AL-1+STR-1"
        juce::String programId;         // npr. "INT-D 049"
        std::array<int, 2> alg {};
        std::vector<juce::uint8> program;   // 4960 bajtova iz PCG-a
        // zvuk sa druge klavijature / modula (bilo koji MIDI instrument): bira se Bank Select + Program Change
        bool external = false;
        juce::String extName;
        // drum kit program: bira se na Kronosu (Program Change), svaki taster se snima posebno
        bool drum = false;
        int msb = -1, lsb = -1, pc = -1, layers = 1;
        juce::uint32 progBank = 0; int progNum = -1;     // EXi: program u PCG-u (bira se na Kronosu pre slanja)
        // procena (dugme Proceni): koliko drzati tone, release, da li treba loop
        bool analyzed = false, anLoop = true, anVelFlat = false;
        double anHold = 0, anRelease = 0;
        juce::String anText;
        juce::String label() const
        {
            if (external) return "EXT " + extName;
            if (drum) return "DRUM " + programId;
            if (exi) return "EXi " + engine + " " + programId;
            const int type = zone[0] & 3, bankByte = zone[16], index = zone[18] | (zone[19] << 8);
            const int bank = bankByte >> 1;
            const bool stereo = (bankByte & 1) != 0;
            juce::String s = type == 2 ? juce::String ("WaveSeq") : (bank == 0 ? juce::String ("ROM") : bank == 1 ? juce::String ("RAM") : "EXs" + juce::String (bank - 1));
            if (stereo && type != 2) s << " St";
            s << " " << juce::String (index).paddedLeft ('0', 4);
            if (type == 1)
                if (auto nm = kronosMsName (bankByte, index); nm.isNotEmpty()) s << " " << nm;
            return s;
        }
    };
}

//==============================================================================
void installCrashLog (const wchar_t* path);
void crashLogThreadStart();
void crashLogNote (const char* text);

class Recorder : public juce::AudioIODeviceCallback
{
public:
    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override
    {
        sampleRate = dev->getCurrentSampleRate();
        juce::Logger::writeToLog ("audio start: " + dev->getName() + "  " + juce::String (sampleRate) + " Hz, " + juce::String (dev->getCurrentBufferSizeSamples()) + " samples"
                                  + (recording.load() ? "  (WHILE RECORDING)" : ""));
        recording = false;
        writePos = 0;
        buffer.setSize (2, (int) (sampleRate * 90.0));
    }
    void audioDeviceStopped() override { juce::Logger::writeToLog ("audio stopped"); }
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c)
            if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);

        float pk = 0.0f;
        for (int c = 0; c < juce::jmin (2, numIn); ++c)
            if (in[c] != nullptr)
                pk = juce::jmax (pk, juce::FloatVectorOperations::findMaximum (in[c], n), -juce::FloatVectorOperations::findMinimum (in[c], n));
        level = juce::jmax (pk, level.load() * 0.9f);

        if (! recording.load()) return;
        const int pos = writePos.load();
        const int todo = juce::jmin (n, buffer.getNumSamples() - pos);
        if (todo <= 0) return;
        for (int c = 0; c < 2; ++c)
        {
            const float* src = in[juce::jmin (c, numIn - 1)];
            if (numIn > 0 && src != nullptr) buffer.copyFrom (c, pos, src, todo);
            else buffer.clear (c, pos, todo);
        }
        writePos = pos + todo;
    }

    juce::AudioBuffer<float> buffer;
    std::atomic<bool> recording { false };
    std::atomic<int> writePos { 0 };
    std::atomic<float> level { 0.0f };
    double sampleRate = 48000.0;
};

//==============================================================================
class MainComponent : public juce::Component,
                      private juce::Thread,
                      private juce::Timer
{
public:
    MainComponent() : juce::Thread ("autosampler")
    {
        juce::Logger::writeToLog ("MainComponent: audio");
        // podesavanja se pamte (%APPDATA%\\LiveSampler\\autosampler.settings)
        {
            juce::PropertiesFile::Options o;
            o.applicationName = "autosampler";
            o.filenameSuffix = ".settings";
            o.folderName = "LiveSampler";
            o.osxLibrarySubFolder = "Application Support";
            props = std::make_unique<juce::PropertiesFile> (o);
        }
        auto savedAudio = props->getXmlValue ("audio");
        const auto err = deviceManager.initialise (2, 2, savedAudio.get(), true);
        if (savedAudio == nullptr) chooseKronosAudioInput();
        if (err.isNotEmpty()) juce::Logger::writeToLog ("audio error: " + err);
        deviceManager.addAudioCallback (&recorder);

        auto styleTitle = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            l.setColour (juce::Label::textColourId, accent);
            addAndMakeVisible (l);
        };
        styleTitle (title1, juce::String::fromUTF8 ("1. WHAT TO RESAMPLE"));
        styleTitle (title2, juce::String::fromUTF8 ("2. TEMPLATE FROM KRONOS (SysEx, nothing is written to the Kronos; needed only for ROM multisamples)"));
        styleTitle (title3, "3. RECORDING");

        loadButton.setButtonText (juce::String::fromUTF8 ("Load PCG..."));
        loadButton.onClick = [this] { choosePcg(); };
        addAndMakeVisible (loadButton);
        fileLabel.setColour (juce::Label::textColourId, lcd);
        addAndMakeVisible (fileLabel);

        search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Search program / combi..."), dim);
        search.onTextChange = [this] { rebuildSource(); };
        addAndMakeVisible (search);
        showCombis.setButtonText ("Combis");
        showCombis.onClick = [this] { rebuildSource(); };
        addAndMakeVisible (showCombis);

        sourceModel.owner = this;
        neededModel.owner = this;
        sourceList.setModel (&sourceModel);
        sourceList.setMultipleSelectionEnabled (true);
        sourceList.setColour (juce::ListBox::backgroundColourId, bg);
        addAndMakeVisible (sourceList);
        sourceModel.owner = this;

        externalButton.setButtonText (juce::String::fromUTF8 ("+ Sound from another keyboard..."));
        externalButton.setTooltip (juce::String::fromUTF8 ("Sampling any MIDI instrument (Yamaha, Roland, module...): choose the MIDI output to it and the audio input from it in step 3."));
        externalButton.onClick = [this] { addExternal(); };
        addAndMakeVisible (externalButton);

        romButton.setButtonText ("+ ROM / EXs multisample...");
        romButton.setTooltip ("Add Kronos ROM or EXs multisamples directly from the list (without a program from the PCG)");
        romButton.onClick = [this] { showRomPicker(); };
        addAndMakeVisible (romButton);

        addButton.setButtonText (juce::String::fromUTF8 ("Add selected →"));
        addButton.onClick = [this] { addSelected(); };
        addAndMakeVisible (addButton);
        clearButton.setButtonText (juce::String::fromUTF8 ("Clear list"));
        clearButton.onClick = [this]
        {
            if (isThreadRunning()) { busyInfo(); return; }
            const juce::ScopedLock sl (jobsLock);
            needed.clear(); exiVels.clear(); neededList.updateContent(); neededList.repaint(); updateInfo();
        };
        addAndMakeVisible (clearButton);
        removeButton.setButtonText ("Remove selected");
        removeButton.onClick = [this]
        {
            if (isThreadRunning()) { busyInfo(); return; }
            auto sel = neededList.getSelectedRows();
            const juce::ScopedLock sl (jobsLock);
            for (int i = sel.size(); --i >= 0;)
                if (sel[i] < (int) needed.size())
                {
                    needed.erase (needed.begin() + sel[i]);
                    if (sel[i] < (int) exiVels.size()) exiVels.erase (exiVels.begin() + sel[i]);
                }
            neededList.deselectAllRows();
            neededList.updateContent();
            updateInfo();
        };
        addAndMakeVisible (removeButton);

        neededList.setModel (&neededModel);
        neededList.setMultipleSelectionEnabled (true);
        neededList.setColour (juce::ListBox::backgroundColourId, bg);
        neededModel.owner = this;
        addAndMakeVisible (neededList);
        info.setColour (juce::Label::textColourId, dim);
        info.setFont (juce::FontOptions (12.5f));
        info.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (info);

        // 2. PCG
        templateLabel.setText (juce::String::fromUTF8 ("Template (initialized program):"), juce::dontSendNotification);
        bankLabel.setText ("Bank for new programs:", juce::dontSendNotification);
        startLabel.setText ("From number:", juce::dontSendNotification);
        for (auto* l : { &templateLabel, &bankLabel, &startLabel }) { l->setColour (juce::Label::textColourId, dim); addAndMakeVisible (l); }
        addAndMakeVisible (templateBox);
        addAndMakeVisible (bankBox);
        startNum.setRange (0, 127, 1);
        startNum.setValue (0);
        startNum.setSliderStyle (juce::Slider::IncDecButtons);
        startNum.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 24);
        addAndMakeVisible (startNum);
        makePcgButton.setButtonText ("Create PCG");
        makePcgButton.onClick = [this] { makePcg(); };
        // snimanje preko PCG-a (USB flash) je izbaceno: sve ide direktno preko SysEx-a
        for (juce::Component* c : { (juce::Component*) &templateLabel, (juce::Component*) &templateBox, (juce::Component*) &bankLabel,
                                    (juce::Component*) &bankBox, (juce::Component*) &startLabel, (juce::Component*) &startNum,
                                    (juce::Component*) &makePcgButton })
            c->setVisible (false);

        // 3. snimanje
        audioButton.setButtonText (juce::String::fromUTF8 ("Audio input (Kronos USB)..."));
        audioButton.onClick = [this] { showAudioSettings(); };
        addAndMakeVisible (audioButton);
        midiLabel.setText ("MIDI output to Kronos:", juce::dontSendNotification);
        addAndMakeVisible (midiOut);
        refreshMidi();
        midiOut.onChange = [this] { openMidi(); };

        midiInLabel.setText ("MIDI input from Kronos:", juce::dontSendNotification);
        midiInLabel.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (midiInLabel);
        addAndMakeVisible (midiInBox);
        inCallback.owner = this;
        midiInDevices = juce::MidiInput::getAvailableDevices();
        for (int i = 0; i < midiInDevices.size(); ++i)
        {
            midiInBox.addItem (midiInDevices[i].name, i + 1);
            if (midiInBox.getSelectedId() == 0 && midiInDevices[i].name.containsIgnoreCase ("kronos")) midiInBox.setSelectedId (i + 1, juce::dontSendNotification);
        }
        midiInBox.onChange = [this] { openMidiIn(); };
        openMidiIn();

        sysexMode.setButtonText (juce::String::fromUTF8 ("Directly via SysEx (changes the current program, nothing is written to the Kronos)"));
        sysexMode.setToggleState (true, juce::dontSendNotification);
        sysexMode.setVisible (false);
        readTemplateButton.setButtonText (juce::String::fromUTF8 ("Read template from Kronos"));
        readTemplateButton.setTooltip (juce::String::fromUTF8 ("Switches the Kronos by itself: ROM in the list -> Init Program from the PCG (e.g. USER-FF 006) and reads it as the template; only EXi -> EXi program. Needs the PCG loaded and GLOBAL > MIDI Program/Bank Change receive on."));
        readTemplateButton.onClick = [this] { readTemplateFromKronos(); };
        addAndMakeVisible (readTemplateButton);
        templateInfo.setColour (juce::Label::textColourId, lcd);
        templateInfo.setText (juce::String::fromUTF8 ("Template not read yet"), juce::dontSendNotification);
        addAndMakeVisible (templateInfo);

        struct F { juce::Label* l; juce::Slider* s; const char* name; double lo, hi, def; };
        const F fields[] = { { &chLabel, &channel, "MIDI channel", 1, 16, 1 }, { &cc0Label, &cc0, "Bank CC0", 0, 127, 0 },
                             { &cc32Label, &cc32, "Bank CC32", 0, 127, 0 }, { &lowLabel, &lowNote, "From note", 0, 127, 24 },
                             { &highLabel, &highNote, "To note", 0, 127, 108 }, { &stepLabel, &step, "Step (semit.)", 1, 12, 3 },
                             { &velLabel, &velocity, "Velocity", 1, 127, 127 }, { &holdLabel, &hold, "Hold (s)", 0.5, 30, 6 },
                             { &relLabel, &release, "Release max (s)", 0.2, 20, 3 } };
        for (auto& f : fields)
        {
            f.l->setText (juce::String::fromUTF8 (f.name), juce::dontSendNotification);
            f.l->setColour (juce::Label::textColourId, dim);
            addAndMakeVisible (f.l);
            f.s->setRange (f.lo, f.hi, (f.hi - f.lo) > 40 || f.lo >= 1 ? (f.hi <= 30 && f.lo < 1 ? 0.1 : 1) : 0.1);
            f.s->setValue (f.def);
            f.s->setSliderStyle (juce::Slider::IncDecButtons);
            f.s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 56, 24);
            addAndMakeVisible (f.s);
        }
        hold.setRange (0.5, 30, 0.5);
        release.setRange (0.2, 20, 0.1);
        lowNote.textFromValueFunction = [] (double v) { return noteName ((int) v); };
        highNote.textFromValueFunction = [] (double v) { return noteName ((int) v); };
        lowNote.updateText(); highNote.updateText();

        midiLabel.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (midiLabel);

        exiIfxToggle.setButtonText (juce::String::fromUTF8 ("EXi: record with insert effects (IFX)"));
        exiIfxToggle.setTooltip (juce::String::fromUTF8 ("Off: dry synth sound (you add effects in Pikaso Workstation). On: sound with the program's IFX effects (chorus, phaser, delay...). Master effects (reverb) are always off."));
        addAndMakeVisible (exiIfxToggle);
        exiVelLabel.setText (juce::String::fromUTF8 ("EXi velocities:"), juce::dontSendNotification);
        exiVelLabel.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (exiVelLabel);
        exiVelBox.addItem (juce::String::fromUTF8 ("Auto (piano 6, EP 5, STR-1 4, organ 1, synth 2)"), 1);
        for (int n : { 1, 2, 3, 4, 6, 8 }) exiVelBox.addItem (juce::String (n) + (n == 1 ? juce::String::fromUTF8 (" velocity (127)") : juce::String::fromUTF8 (" velocities")), 100 + n);
        exiVelBox.setSelectedId (1, juce::dontSendNotification);
        exiVelBox.onChange = [this] { updateInfo(); };
        addAndMakeVisible (exiVelBox);

        autoLoopToggle.setButtonText (juce::String::fromUTF8 ("Auto loop (for sustained sounds)"));
        autoLoopToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (autoLoopToggle);
        zoneToggle.setButtonText ("ROM: one note per zone");
        zoneToggle.setTooltip ("For ROM multisamples: first plays every key briefly and finds where the multisample changes to the next "
                               "Kronos sample (zone). Then it records only one note per zone (instead of every 3rd note) and writes the "
                               "exact zone map for Pikaso Workstation. Needs a dry Init Program template (no key tracking, LFO or effects).");
        zoneToggle.setToggleState (true, juce::dontSendNotification);
        zoneToggle.onClick = [this] { updateInfo(); };
        addAndMakeVisible (zoneToggle);
        bits16Toggle.setButtonText (juce::String::fromUTF8 ("16-bit WAV (half the disk)"));
        bits16Toggle.setTooltip (juce::String::fromUTF8 ("Pikaso Workstation keeps samples in RAM as 16-bit anyway, so 16-bit files sound the same live and take half the disk. Off = 24-bit."));
        bits16Toggle.onClick = [this] { updateInfo(); };
        addAndMakeVisible (bits16Toggle);
        for (auto [s, l, name, lo, hi, def] : { std::tuple<juce::Slider*, juce::Label*, const char*, double, double, double> { &xfadeMs, &xfadeLabel, "Crossfade (ms)", 0, 1000, 80 },
                                                { &minLoopMs, &minLoopLabel, "Min loop (ms)", 30, 5000, 300 } })
        {
            l->setText (name, juce::dontSendNotification);
            l->setColour (juce::Label::textColourId, dim);
            addAndMakeVisible (l);
            s->setRange (lo, hi, 1);
            s->setValue (def);
            s->setSliderStyle (juce::Slider::IncDecButtons);
            s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 56, 24);
            addAndMakeVisible (s);
        }

        folderButton.setButtonText (juce::String::fromUTF8 ("Sample folder..."));
        folderButton.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (folderButton);
        folderLabel.setColour (juce::Label::textColourId, lcd);
        addAndMakeVisible (folderLabel);

        analyzeButton.setButtonText ("Analyze (selected)");
        analyzeButton.setTooltip (juce::String::fromUTF8 ("Plays 3 notes (low, middle, high) and analyzes: does the sound sustain (loop), how long to hold the key, how long the release is, and whether velocity changes the sound. The values are used when recording that sound."));
        analyzeButton.onClick = [this] { startAnalysis(); };
        addAndMakeVisible (analyzeButton);
        autoAnalyzeToggle.setButtonText (juce::String::fromUTF8 ("Analyze before recording"));
        autoAnalyzeToggle.setTooltip (juce::String::fromUTF8 ("Before recording each sound (not analyzed yet) it first plays 3 notes and sets Hold, Release, loop and velocity layers by itself."));
        autoAnalyzeToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (autoAnalyzeToggle);

        testButton.setButtonText ("Test (first in list)");
        testButton.onClick = [this]
        {
            if (needed.empty()) return;
            if (needed[0].external)
            {
                const auto& n = needed[0];
                const int ch = (int) channel.getValue();
                if (n.msb >= 0) send (juce::MidiMessage::controllerEvent (ch, 0, n.msb));
                if (n.lsb >= 0) send (juce::MidiMessage::controllerEvent (ch, 32, n.lsb));
                if (n.pc >= 0) send (juce::MidiMessage::programChange (ch, n.pc));
                juce::Thread::sleep (500);
                send (juce::MidiMessage::noteOn (ch, 60, (juce::uint8) 100)); juce::Thread::sleep (1200); send (juce::MidiMessage::noteOff (ch, 60));
            }
            else if (needed[0].exi)
            {
                juce::String err;
                if (! ensureKronosKind (needed[0], &err)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "EXi", err); return; }
                if (! sendExiSysex (needed[0], err)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "EXi", err); return; }
                send (juce::MidiMessage::noteOn ((int) channel.getValue(), 60, (juce::uint8) 100)); juce::Thread::sleep (1200); send (juce::MidiMessage::noteOff ((int) channel.getValue(), 60));
            }
            else if (sysexMode.getToggleState()) { juce::String err; if (! ensureKronosKind (needed[0], &err)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "HD-1", err); return; } sendProgramSysex (needed[0]); send (juce::MidiMessage::noteOn ((int) channel.getValue(), 60, (juce::uint8) 127)); juce::Thread::sleep (800); send (juce::MidiMessage::noteOff ((int) channel.getValue(), 60)); }
            else if (needed[0].slot >= 0) selectProgram (needed[0].slot);
        };
        addAndMakeVisible (testButton);
        startButton.setButtonText ("START");
        startButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f7d4a));
        startButton.onClick = [this] { if (isThreadRunning()) { signalThreadShouldExit(); } else startSampling(); };
        addAndMakeVisible (startButton);
        progress.setColour (juce::Label::textColourId, juce::Colours::white);
        progress.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        addAndMakeVisible (progress);
        help.setText (juce::String::fromUTF8 (
            "Order: 1) load a PCG and add programs/combis (or a sound from another keyboard)  2) for ROM multisamples: on the Kronos select "
            "Init Program and click Read template  3) choose the Kronos USB audio input, MIDI output and input, then START. "
            "Recording can be stopped and continued: existing files are skipped.\n"
            "EXi programs (AL-1, CX-3, SGX-2...) go only directly via SysEx (needs the MIDI input from the Kronos too); they need no template. "
            "On the Kronos turn off the KARMA and Drum Track buttons. "
            "Right-click the list on the right: Resample again (deletes the old samples)."),
            juce::dontSendNotification);
        help.setColour (juce::Label::textColourId, dim);
        help.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (help);

        loadSettings();
        startTimerHz (10);
        updateInfo();
        setSize (1180, 880);
        juce::Logger::writeToLog ("MainComponent: gotovo");
    }

    // Live Sampler pokrece: --pcg "<fajl>" --program <bankId>:<broj> ... --combi <bankId>:<broj> ...
    juce::StringArray pendingCommands;   // stiglo dok snima: primenjuje se posle (lista se ne sme menjati dok nit radi)

    void applyCommandLine (const juce::String& commandLine)
    {
        juce::Logger::writeToLog ("command: " + commandLine);
        if (isThreadRunning())
        {
            pendingCommands.add (commandLine);
            setStatus (juce::String::fromUTF8 ("Added sounds will go to the list when the current sampling finishes"));
            return;
        }
        auto args = juce::StringArray::fromTokens (commandLine, true);
        for (auto& a : args) a = a.unquoted();
        const int pi = args.indexOf ("--pcg");
        if (pi < 0 || pi + 1 >= args.size()) return;
        const juce::File f (args[pi + 1]);
        if (! f.existsAsFile()) return;
        if (pcgFile != f || pcg == nullptr) loadPcg (f);
        if (pcg == nullptr) return;
        for (int i = 0; i + 1 < args.size(); ++i)
        {
            const bool isProg = args[i] == "--program", isCombi = args[i] == "--combi";
            if (! isProg && ! isCombi) continue;
            const auto bank = (juce::uint32) args[i + 1].upToFirstOccurrenceOf (":", false, false).getLargeIntValue();
            const int num = args[i + 1].fromFirstOccurrenceOf (":", false, false).getIntValue();
            if (isProg)
            {
                if (auto* p = pcg->findProgram (bank, num))
                {
                    if (! programHasRom (*p))
                        setStatus (p->id() + " " + p->name + juce::String::fromUTF8 (": uses only your own samples (KSC), nothing to resample from the Kronos ROM. "
                                   "The Workstation finds them in the KSC folder (SETTINGS sample folders)."));
                    addProgram (*p);
                }
            }
            else
            {
                for (auto& c : pcg->combis)
                    if (c.bankId == bank && c.number == num)
                        for (auto& t : c.timbres)
                            if (t.play)
                                if (auto* p = pcg->findProgram (t.programBank, t.programNumber)) addProgram (*p);
            }
        }
        neededList.updateContent();
        neededList.repaint();
        updateInfo();
    }

    ~MainComponent() override
    {
        saveSettings();
        stopThread (4000);
        if (midiIn != nullptr) midiIn->stop();
        deviceManager.removeAudioCallback (&recorder);
    }

    // audio ulaz: uredjaj cije ime sadrzi KRONOS (npr. "Line (2- KRONOS)")
    void chooseKronosAudioInput()
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            for (auto& name : type->getDeviceNames (true))
                if (name.containsIgnoreCase ("kronos"))
                {
                    setup.inputDeviceName = name;
                    setup.useDefaultInputChannels = false;
                    setup.inputChannels.clear();
                    setup.inputChannels.setRange (0, 2, true);
                    deviceManager.setAudioDeviceSetup (setup, true);
                    break;
                }
        }
    }

    void saveSettings()
    {
        if (props == nullptr) return;
        if (auto x = deviceManager.createStateXml()) props->setValue ("audio", x.get());
        props->setValue ("channel", (int) channel.getValue());
        props->setValue ("midiOut", midiOut.getText());
        props->setValue ("midiIn", midiInBox.getText());
        props->setValue ("folder", outFolder.getFullPathName());
        props->setValue ("low", (int) lowNote.getValue());
        props->setValue ("high", (int) highNote.getValue());
        props->setValue ("step", (int) step.getValue());
        props->setValue ("hold", hold.getValue());
        props->setValue ("release", release.getValue());
        props->setValue ("zones", zoneToggle.getToggleState());
        props->setValue ("bits16", bits16Toggle.getToggleState());
        props->saveIfNeeded();
    }

    void loadSettings()
    {
        channel.setValue (props->getIntValue ("channel", 1), juce::dontSendNotification);
        lowNote.setValue (props->getIntValue ("low", (int) lowNote.getValue()), juce::dontSendNotification);
        highNote.setValue (props->getIntValue ("high", (int) highNote.getValue()), juce::dontSendNotification);
        step.setValue (props->getIntValue ("step", (int) step.getValue()), juce::dontSendNotification);
        hold.setValue (props->getDoubleValue ("hold", hold.getValue()), juce::dontSendNotification);
        release.setValue (props->getDoubleValue ("release", release.getValue()), juce::dontSendNotification);
        zoneToggle.setToggleState (props->getBoolValue ("zones", true), juce::dontSendNotification);
        bits16Toggle.setToggleState (props->getBoolValue ("bits16", false), juce::dontSendNotification);
        const auto f = props->getValue ("folder");
        if (f.isNotEmpty()) { outFolder = juce::File (f); folderLabel.setText (outFolder.getFullPathName(), juce::dontSendNotification); }
        juce::Timer::callAfterDelay (1500, [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe == nullptr) return;
            if (const int c = safe->tidyRomFolders(); c > 0)
                safe->setStatus (juce::String::fromUTF8 ("Sample folders tidied (") + juce::String (c)
                                 + juce::String::fromUTF8 (" changes): one folder per ROM multisample. In Pikaso Workstation click Refresh (SETTINGS)."));
            safe->neededList.repaint();
        });
        auto pick = [] (juce::ComboBox& b, const juce::String& name)
        {
            if (name.isEmpty()) return false;
            for (int k = 0; k < b.getNumItems(); ++k)
                if (b.getItemText (k) == name) { b.setSelectedItemIndex (k, juce::sendNotification); return true; }
            return false;
        };
        pick (midiOut, props->getValue ("midiOut"));
        pick (midiInBox, props->getValue ("midiIn"));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (bg);
        for (auto r : { sec1, sec2, sec3 })
        {
            g.setColour (panel);
            g.fillRoundedRectangle (r.toFloat(), 8.0f);
        }
        // nivo ulaza
        auto m = meter.toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (m);
        const float lv = juce::jlimit (0.0f, 1.0f, recorder.level.load());
        g.setColour (lv > 0.98f ? juce::Colours::red : juce::Colour (0xff3ddc84));
        g.fillRect (m.withWidth (m.getWidth() * lv));
        g.setColour (dim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("INPUT", m.toNearestInt().translated (0, -14).withHeight (14), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        sec1 = r.removeFromTop (350);
        r.removeFromTop (8);
        sec2 = r.removeFromTop (78);
        r.removeFromTop (8);
        sec3 = r;

        auto a = sec1.reduced (10);
        title1.setBounds (a.removeFromTop (22));
        auto top = a.removeFromTop (30);
        loadButton.setBounds (top.removeFromLeft (140));
        top.removeFromLeft (10);
        fileLabel.setBounds (top);
        a.removeFromTop (6);
        auto left = a.removeFromLeft (a.getWidth() / 2 - 70);
        auto srow = left.removeFromTop (28);
        showCombis.setBounds (srow.removeFromRight (90));
        search.setBounds (srow);
        left.removeFromTop (4);
        sourceList.setBounds (left);
        auto mid = a.removeFromLeft (140).reduced (8, 0);
        addButton.setBounds (mid.removeFromTop (30).translated (0, 60));
        externalButton.setBounds (mid.removeFromBottom (44).withTrimmedBottom (4));
        romButton.setBounds (mid.removeFromBottom (52).withTrimmedBottom (8));
        auto right = a;
        auto infoRow = right.removeFromBottom (28);
        removeButton.setBounds (infoRow.removeFromRight (130));
        infoRow.removeFromRight (6);
        clearButton.setBounds (infoRow.removeFromRight (110));
        info.setBounds (infoRow);
        neededList.setBounds (right.withTrimmedTop (32));

        auto b = sec2.reduced (10);
        title2.setBounds (b.removeFromTop (22));
        auto row = juce::Rectangle<int>();
        templateLabel.setBounds (row.removeFromLeft (210));
        templateBox.setBounds (row.removeFromLeft (290).reduced (0, 2));
        row.removeFromLeft (12);
        bankLabel.setBounds (row.removeFromLeft (160));
        bankBox.setBounds (row.removeFromLeft (110).reduced (0, 2));
        row.removeFromLeft (8);
        startLabel.setBounds (row.removeFromLeft (70));
        startNum.setBounds (row.removeFromLeft (110).reduced (0, 2));
        row.removeFromLeft (12);
        makePcgButton.setBounds (row.removeFromLeft (140));
        b.removeFromTop (4);
        auto rowS = b.removeFromTop (30);
        readTemplateButton.setBounds (rowS.removeFromLeft (210));
        rowS.removeFromLeft (10);
        templateInfo.setBounds (rowS);

        auto c = sec3.reduced (10);
        title3.setBounds (c.removeFromTop (22));
        auto r1 = c.removeFromTop (32);
        audioButton.setBounds (r1.removeFromLeft (210));
        r1.removeFromLeft (12);
        midiLabel.setBounds (r1.removeFromLeft (150));
        midiOut.setBounds (r1.removeFromLeft (220).reduced (0, 2));
        r1.removeFromLeft (12);
        midiInLabel.setBounds (r1.removeFromLeft (150));
        midiInBox.setBounds (r1.removeFromLeft (220).reduced (0, 2));
        r1.removeFromLeft (12);
        meter = r1.removeFromLeft (160).reduced (0, 10).withTrimmedTop (6);
        c.removeFromTop (8);
        juce::Label* ls[] = { &chLabel, &cc0Label, &cc32Label, &lowLabel, &highLabel, &stepLabel, &velLabel, &holdLabel, &relLabel };
        juce::Slider* ss[] = { &channel, &cc0, &cc32, &lowNote, &highNote, &step, &velocity, &hold, &release };
        for (int row2 = 0; row2 < 2; ++row2)
        {
            auto rr = c.removeFromTop (30);
            for (int i = row2 * 5; i < juce::jmin (9, row2 * 5 + 5); ++i)
            {
                ls[i]->setBounds (rr.removeFromLeft (110));
                ss[i]->setBounds (rr.removeFromLeft (110).reduced (0, 2));
                rr.removeFromLeft (10);
            }
            c.removeFromTop (4);
        }
        auto rl = c.removeFromTop (30);
        autoLoopToggle.setBounds (rl.removeFromLeft (240));
        xfadeLabel.setBounds (rl.removeFromLeft (110));
        xfadeMs.setBounds (rl.removeFromLeft (110).reduced (0, 2));
        rl.removeFromLeft (10);
        minLoopLabel.setBounds (rl.removeFromLeft (110));
        minLoopMs.setBounds (rl.removeFromLeft (110).reduced (0, 2));
        rl.removeFromLeft (20);
        zoneToggle.setBounds (rl.removeFromLeft (260));
        c.removeFromTop (4);
        auto re = c.removeFromTop (30);
        exiIfxToggle.setBounds (re.removeFromLeft (300));
        exiVelLabel.setBounds (re.removeFromLeft (80));
        exiVelBox.setBounds (re.removeFromLeft (360).reduced (0, 2));
        re.removeFromLeft (20);
        bits16Toggle.setBounds (re.removeFromLeft (240));
        c.removeFromTop (4);
        auto r3 = c.removeFromTop (32);
        folderButton.setBounds (r3.removeFromLeft (180));
        r3.removeFromLeft (10);
        folderLabel.setBounds (r3);
        c.removeFromTop (8);
        auto r4 = c.removeFromTop (40);
        testButton.setBounds (r4.removeFromLeft (170));
        r4.removeFromLeft (8);
        analyzeButton.setBounds (r4.removeFromLeft (150));
        r4.removeFromLeft (8);
        autoAnalyzeToggle.setBounds (r4.removeFromLeft (200));
        r4.removeFromLeft (8);
        startButton.setBounds (r4.removeFromLeft (140));
        r4.removeFromLeft (14);
        progress.setBounds (r4);
        c.removeFromTop (6);
        help.setBounds (c);
    }

private:
    //==========================================================================
    struct SourceModel : public juce::ListBoxModel
    {
        MainComponent* owner = nullptr;
        int getNumRows() override { return (int) owner->sourceRows.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
        {
            if (sel) g.fillAll (lcd.withAlpha (0.25f));
            if (row < 0 || row >= (int) owner->sourceRows.size()) return;
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (owner->sourceText (owner->sourceRows[(size_t) row]), 6, 0, w - 8, h, juce::Justification::centredLeft, true);
        }
    } sourceModel;

    struct NeededModel : public juce::ListBoxModel
    {
        MainComponent* owner = nullptr;
        int getNumRows() override { return (int) owner->needed.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
        {
            if (sel) g.fillAll (lcd.withAlpha (0.25f));
            if (row < 0 || row >= (int) owner->needed.size()) return;
            const auto& n = owner->needed[(size_t) row];
            g.setColour (owner->isDone (n) ? juce::Colour (0xff3ddc84) : juce::Colours::white);
            g.setFont (juce::FontOptions (13.0f));
            juce::String t = n.label();
            if (n.slot >= 0) t << "   -> program " << juce::String (n.slot).paddedLeft ('0', 3);
            if (n.anText.isNotEmpty()) t << "   [" << n.anText << "]";
            t << "   (" << n.usedBy << ")";
            g.drawText (t, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
        }
        void listBoxItemClicked (int row, const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) owner->neededMenu (row);
        }
    } neededModel;

    // zvuk sa druge klavijature: ime + (po zelji) Bank MSB/LSB i Program Change + broj jacina
    void addExternal()
    {
        auto* aw = new juce::AlertWindow (juce::String::fromUTF8 ("Sound from another keyboard"),
            juce::String::fromUTF8 ("Any MIDI instrument. In step 3 choose the MIDI output to that instrument, its MIDI channel and the audio input it is connected to.\n"
                                    "Leave Bank / Program empty if you select the sound yourself on the instrument."), juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", "", "Sound name:");
        aw->addTextEditor ("msb", "", "Bank MSB (CC0):");
        aw->addTextEditor ("lsb", "", "Bank LSB (CC32):");
        aw->addTextEditor ("pc", "", "Program (1-128):");
        aw->addComboBox ("layers", { "1", "2", "3", "4", "6", "8" }, juce::String::fromUTF8 ("Velocities (velocity layers):"));
        if (auto* cb = aw->getComboBoxComponent ("layers")) cb->setSelectedItemIndex (2);
        aw->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton (juce::String::fromUTF8 ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<MainComponent> safe (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r)
        {
            if (safe == nullptr || r != 1) return;
            const auto name = aw->getTextEditorContents ("name").trim();
            if (name.isEmpty()) return;
            auto num = [aw] (const char* id, int lo, int hi, int offset)
            {
                const auto t = aw->getTextEditorContents (id).trim();
                return t.isEmpty() ? -1 : juce::jlimit (lo, hi, t.getIntValue() - offset);
            };
            NeededMs n;
            n.external = true;
            n.extName = name;
            n.msb = num ("msb", 0, 127, 0);
            n.lsb = num ("lsb", 0, 127, 0);
            n.pc = num ("pc", 0, 127, 1);
            const int layerChoices[] = { 1, 2, 3, 4, 6, 8 };
            if (auto* cb = aw->getComboBoxComponent ("layers")) n.layers = layerChoices[juce::jlimit (0, 5, cb->getSelectedItemIndex())];
            n.romKey = "ext:" + name.toLowerCase() + ":" + juce::String (n.msb) + ":" + juce::String (n.lsb) + ":" + juce::String (n.pc);
            n.usedBy = (n.pc >= 0 ? "PC " + juce::String (n.pc + 1) : juce::String ("no PC")) + (n.msb >= 0 || n.lsb >= 0 ? " / bank " + juce::String (n.msb) + "-" + juce::String (n.lsb) : juce::String());
            for (auto& x : safe->needed) if (x.romKey == n.romKey) return;
            safe->pushJob (n);
            safe->neededList.updateContent();
            safe->neededList.repaint();
            safe->updateInfo();
        }), true);
    }

    void askMergeDuplicates()
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Merge duplicate folders",
            juce::String::fromUTF8 ("Sounds recorded twice (e.g. old 24-bit folder and a new \"... manual\" folder) are merged:\n"
                                    "the old folder name stays (Pikaso Workstation users point to it), the newest samples go into it, "
                                    "the old samples and the extra folder go to the Recycle Bin.\n\nThen click Refresh in Pikaso Workstation (SETTINGS)."),
            "Merge", "Cancel", nullptr,
            juce::ModalCallbackFunction::create ([safe] (int ok)
            {
                if (safe == nullptr || ok == 0) return;
                juce::StringArray log;
                const int n = safe->mergeDuplicateFolders (log);
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Merge duplicate folders",
                    n == 0 ? juce::String::fromUTF8 ("No duplicates found.")
                           : juce::String (n) + juce::String::fromUTF8 (" merged:\n\n") + log.joinIntoString ("\n"));
                safe->neededList.repaint();
                safe->updateInfo();
            }));
    }

    // desni klik na listu: ponovo snimi (brise stare semplove tog programa / multisempla)
    void neededMenu (int row)
    {
        if (row < 0 || row >= (int) needed.size()) return;
        if (! neededList.isRowSelected (row)) neededList.selectRow (row);
        auto sel = neededList.getSelectedRows();
        juce::PopupMenu m;
        m.addItem (1, juce::String::fromUTF8 ("Resample again (delete the old samples)"), ! isThreadRunning());
        m.addItem (2, "Remove from list", ! isThreadRunning());
        m.addSeparator();
        m.addItem (3, juce::String::fromUTF8 ("Merge duplicate folders (same sound twice: keep the newest samples)"), ! isThreadRunning());
        juce::Component::SafePointer<MainComponent> safe (this);
        m.showMenuAsync ({}, [safe, sel] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == 2) { safe->removeButton.triggerClick(); return; }
            if (r == 3) { safe->askMergeDuplicates(); return; }
            juce::StringArray names;
            for (int i = 0; i < sel.size(); ++i)
                if (sel[i] < (int) safe->needed.size()) names.add (safe->needed[(size_t) sel[i]].label());
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Resample again",
                juce::String::fromUTF8 ("I will delete the recorded samples for:\n\n") + names.joinIntoString ("\n")
                + juce::String::fromUTF8 ("\n\nThen click START to record them again. In Pikaso Workstation click Refresh (SETTINGS) when it is recorded."),
                juce::String::fromUTF8 ("Delete"), juce::String::fromUTF8 ("Cancel"), nullptr,
                juce::ModalCallbackFunction::create ([safe, sel] (int ok)
                {
                    if (safe == nullptr || ok == 0) return;
                    for (int i = 0; i < sel.size(); ++i)
                        if (sel[i] < (int) safe->needed.size())
                            safe->msFolder (safe->needed[(size_t) sel[i]]).deleteRecursively();
                    safe->neededList.repaint();
                    safe->updateInfo();
                }));
        });
    }

    //==========================================================================
    void choosePcg()
    {
        chooser = std::make_unique<juce::FileChooser> ("Kronos PCG", juce::File ("C:\\KRONOS"), "*.PCG;*.pcg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) loadPcg (fc.getResult()); });
    }

    void loadPcg (const juce::File& f)
    {
        if (isThreadRunning()) { busyInfo(); return; }   // nit koristi PCG: ne sme se zameniti dok snima
        juce::String err;
        auto p = std::make_unique<Kronos::Pcg>();
        if (! p->load (f, err) || ! f.loadFileAsData (pcgData))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "PCG", err);
            return;
        }
        pcg = std::move (p);
        pcgFile = f;
        banks = findProgramBanks (pcgData);
        fileLabel.setText (f.getFullPathName() + "   (" + juce::String ((int) pcg->programs.size()) + " programs, "
                           + juce::String ((int) pcg->combis.size()) + " combis, Kronos MIDI channel " + juce::String (pcg->globalChannel) + ")",
                           juce::dontSendNotification);
        if (outFolder == juce::File() || props->getValue ("folder").isEmpty())
        {
            outFolder = f.getParentDirectory().getChildFile ("KRONOS ROM");
            folderLabel.setText (outFolder.getFullPathName(), juce::dontSendNotification);
        }

        templateBox.clear();
        int initIdx = -1;
        for (size_t i = 0; i < pcg->programs.size(); ++i)
        {
            const auto& pr = pcg->programs[i];
            if (! pr.hd1) continue;
            templateBox.addItem (pr.id() + "  " + pr.name, (int) i + 1);
            if (initIdx < 0 && pr.name.startsWithIgnoreCase ("init")) initIdx = (int) i;
        }
        if (initIdx >= 0) templateBox.setSelectedId (initIdx + 1);
        else templateBox.setSelectedItemIndex (0);

        bankBox.clear();
        for (auto& b : banks)
            if ((b.bankId >> 16) == 2 && ! b.exi)
                bankBox.addItem (Kronos::programBankName (b.bankId), (int) b.bankId + 1);
        bankBox.setSelectedItemIndex (juce::jmax (0, bankBox.getNumItems() - 1));
        rebuildSource();
    }

    juce::String sourceText (int code) const
    {
        if (code >= 0 && code < 100000 && pcg->programs[(size_t) code].exi)
        {
            const auto& p = pcg->programs[(size_t) code];
            return p.id() + "  " + p.name + "   [EXi " + p.engineText() + "]";
        }
        if (code >= 100000)
        {
            const auto& c = pcg->combis[(size_t) (code - 100000)];
            return "COMBI " + c.id() + "  " + c.name;
        }
        const auto& p = pcg->programs[(size_t) code];
        return p.id() + "  " + p.name;
    }

    bool programHasRom (const Kronos::Program& p) const
    {
        if (p.exi) return true;
        if (p.drumKit) return p.drumKey.isNotEmpty();
        if (! p.hd1) return false;
        for (int o = 0; o < (p.doubleMode ? 2 : 1); ++o)
            for (auto& z : p.osc[(size_t) o])
                if (z.used && z.romKey.isNotEmpty()) return true;
        return false;
    }

    void rebuildSource()
    {
        sourceRows.clear();
        if (pcg == nullptr) return;
        const auto q = search.getText().trim();
        if (showCombis.getToggleState())
        {
            for (size_t i = 0; i < pcg->combis.size(); ++i)
            {
                const auto& c = pcg->combis[i];
                bool any = false;
                for (auto& t : c.timbres)
                    if (t.play)
                        if (auto* p = pcg->findProgram (t.programBank, t.programNumber); p != nullptr && programHasRom (*p)) any = true;
                if (any && (q.isEmpty() || c.name.containsIgnoreCase (q))) sourceRows.push_back (100000 + (int) i);
            }
        }
        else
        {
            for (size_t i = 0; i < pcg->programs.size(); ++i)
            {
                const auto& p = pcg->programs[i];
                if (programHasRom (p) && (q.isEmpty() || p.name.containsIgnoreCase (q))) sourceRows.push_back ((int) i);
            }
        }
        sourceList.updateContent();
        sourceList.repaint();
    }

    void addProgram (const Kronos::Program& p)
    {
        if (! programHasRom (p)) return;
        if (p.exi)
        {
            for (auto& n : needed)
                if (n.romKey == p.exiKey) return;
            auto* raw = rawProgram (p);
            if (raw == nullptr) return;
            NeededMs n;
            n.exi = true;
            n.romKey = p.exiKey;
            n.engine = p.engineText();
            n.alg = p.exiAlg;
            n.programId = p.id();
            n.progBank = p.bankId;
            n.progNum = p.number;
            n.usedBy = p.name;
            n.program.assign (raw, raw + Kronos::programSize);
            pushJob (std::move (n));
            return;
        }
        if (p.drumKit)
        {
            for (auto& n : needed)
                if (n.romKey == p.drumKey) return;
            NeededMs n;
            n.drum = true;
            n.romKey = p.drumKey;
            n.programId = p.id();
            n.progBank = p.bankId;
            n.progNum = p.number;
            n.usedBy = p.name;
            pushJob (std::move (n));
            return;
        }
        for (int o = 0; o < (p.doubleMode ? 2 : 1); ++o)
            for (int z = 0; z < 8; ++z)
            {
                const auto& zone = p.osc[(size_t) o][(size_t) z];
                if (! zone.used || zone.romKey.isEmpty()) continue;
                bool have = false;
                for (auto& n : needed)
                    if (n.romKey == zone.romKey) { have = true; break; }
                if (have) continue;
                NeededMs n;
                n.romKey = zone.romKey;
                juce::MemoryBlock hex;
                hex.loadFromHexString (zone.romKey.fromFirstOccurrenceOf ("rom:", false, false));
                for (int k = 0; k < 20 && k < (int) hex.getSize(); ++k) n.zone[(size_t) k] = ((const juce::uint8*) hex.getData())[k];
                n.usedBy = p.name;
                pushJob (n);
            }
    }

    // ROM / EXs multisempl direktno sa spiska (bez programa iz PCG-a)
    static juce::String romKeyFor (int bankByte, int index)
    {
        std::array<juce::uint8, 20> z {};
        z[0] = 1; z[1] = 'K'; z[2] = 'O'; z[3] = 'R'; z[4] = 'G'; z[13] = 'M'; z[14] = 'S';
        z[16] = (juce::uint8) bankByte; z[18] = (juce::uint8) (index & 0xff); z[19] = (juce::uint8) ((index >> 8) & 0xff);
        return "rom:" + juce::String::toHexString (z.data(), 20, 0);
    }

    void addRomMultisample (int bankByte, int index)
    {
        const auto key = romKeyFor (bankByte, index);
        for (auto& n : needed) if (n.romKey == key) return;
        NeededMs n;
        n.romKey = key;
        juce::MemoryBlock hex;
        hex.loadFromHexString (key.fromFirstOccurrenceOf ("rom:", false, false));
        for (int k = 0; k < 20 && k < (int) hex.getSize(); ++k) n.zone[(size_t) k] = ((const juce::uint8*) hex.getData())[k];
        n.usedBy = "manual";
        pushJob (n);
    }

    struct RomPicker : public juce::Component, private juce::ListBoxModel
    {
        struct Item { int bankByte, index; juce::String label; };
        RomPicker (MainComponent& o) : owner (o)
        {
            const int banks[] = { 0, 1, 2, 3, 4, 5, 6, 7, 17, 18, 22, 23, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218 };
            for (int exs : banks)
                for (int st = 0; st < 2; ++st)
                {
                    const int bankByte = ((exs == 0 ? 0 : exs + 1) << 1) | st;
                    for (int i = 0; i < 1600; ++i)
                    {
                        const auto nm = kronosMsName (bankByte, i);
                        if (nm.isEmpty()) { if (i > 240) break; continue; }
                        all.push_back ({ bankByte, i, (exs == 0 ? juce::String ("ROM") : "EXs" + juce::String (exs)) + (st ? " St" : "") + " "
                                                       + juce::String (i).paddedLeft ('0', 4) + "  " + nm });
                    }
                }
            // vec presemplovani (u folderu za semplove postoji kronos-ms.txt sa tim kljucem): zelenim slovima
            for (auto& d : owner.outFolder.findChildFiles (juce::File::findDirectories, false))
            {
                const auto tag = d.getChildFile ("kronos-ms.txt");
                if (tag.existsAsFile()) done.insert (juce::StringArray::fromLines (tag.loadFileAsString())[0].trim().toLowerCase());
            }
            search.setTextToShowWhenEmpty ("Search (e.g. trumpet, sax, guitar)...", juce::Colours::grey);
            search.onTextChange = [this] { filter(); };
            addAndMakeVisible (search);
            list.setModel (this);
            list.setMultipleSelectionEnabled (true);
            list.setRowHeight (22);
            addAndMakeVisible (list);
            add.setButtonText ("Add selected to the list");
            add.onClick = [this]
            {
                auto sel = list.getSelectedRows();
                for (int k = 0; k < sel.size(); ++k)
                    if (sel[k] < (int) shown.size()) owner.addRomMultisample (all[(size_t) shown[(size_t) sel[k]]].bankByte, all[(size_t) shown[(size_t) sel[k]]].index);
                owner.neededList.updateContent(); owner.neededList.repaint(); owner.updateInfo();
                info.setText (juce::String (sel.size()) + " added", juce::dontSendNotification);
            };
            addAndMakeVisible (add);
            info.setText ("Green = already resampled. Ctrl / Shift + click to select several. EXs: only the expansions installed on your Kronos will sound.", juce::dontSendNotification);
            addAndMakeVisible (info);
            filter();
            setSize (620, 640);
        }
        void filter()
        {
            shown.clear();
            const auto words = juce::StringArray::fromTokens (search.getText().trim(), " ", "");
            for (size_t i = 0; i < all.size(); ++i)
            {
                bool ok = true;
                for (auto& w : words) if (w.isNotEmpty() && ! all[i].label.containsIgnoreCase (w)) { ok = false; break; }
                if (ok) shown.push_back ((int) i);
            }
            list.updateContent(); list.deselectAllRows(); list.repaint();
        }
        int getNumRows() override { return (int) shown.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
        {
            if (row < 0 || row >= (int) shown.size()) return;
            if (sel) g.fillAll (juce::Colour (0xff3a5f8a));
            const auto& it = all[(size_t) shown[(size_t) row]];
            const bool isDone = done.count (MainComponent::romKeyFor (it.bankByte, it.index).toLowerCase()) > 0;
            g.setColour (isDone ? juce::Colour (0xff5fd36b) : juce::Colours::white);
            g.setFont (juce::FontOptions (14.0f, isDone ? juce::Font::bold : juce::Font::plain));
            g.drawText (it.label + (isDone ? juce::String::fromUTF8 ("   \xe2\x9c\x93 resampled") : juce::String()), 6, 0, w - 8, h, juce::Justification::centredLeft, true);
        }
        void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { add.triggerClick(); }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            search.setBounds (r.removeFromTop (28));
            r.removeFromTop (6);
            auto b = r.removeFromBottom (32);
            add.setBounds (b.removeFromRight (220));
            info.setBounds (b);
            r.removeFromBottom (6);
            list.setBounds (r);
        }
        MainComponent& owner;
        std::vector<Item> all;
        std::vector<int> shown;
        std::set<juce::String> done;
        juce::TextEditor search;
        juce::ListBox list;
        juce::TextButton add;
        juce::Label info;
    };

    void showRomPicker()
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (new RomPicker (*this));
        o.dialogTitle = "Kronos ROM / EXs multisamples";
        o.dialogBackgroundColour = juce::Colour (0xff1a1d24);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.componentToCentreAround = this;
        o.launchAsync();
    }

    void addSelected()
    {
        if (pcg == nullptr) return;
        if (isThreadRunning()) { busyInfo(); return; }
        auto sel = sourceList.getSelectedRows();
        for (int i = 0; i < sel.size(); ++i)
        {
            const int code = sourceRows[(size_t) sel[i]];
            if (code >= 100000)
            {
                for (auto& t : pcg->combis[(size_t) (code - 100000)].timbres)
                    if (t.play)
                        if (auto* p = pcg->findProgram (t.programBank, t.programNumber)) addProgram (*p);
            }
            else addProgram (pcg->programs[(size_t) code]);
        }
        neededList.updateContent();
        neededList.repaint();
        updateInfo();
    }

    // procena: vreme (s) i disk (bajtovi) za ono sto nije snimljeno
    juce::int64 estimateBytes (int seconds) const
    {
        double sr = 48000.0;
        if (auto* d = deviceManager.getCurrentAudioDevice()) sr = d->getCurrentSampleRate();
        // snima se oko 70% vremena (ostalo je biranje zvuka, cekanje tisine)
        return (juce::int64) (seconds * 0.7 * sr * 2.0 * (bits16Toggle.getToggleState() ? 2 : 3));
    }

    static juce::String fmtTime (int s) { return s >= 3600 ? juce::String (s / 3600) + " h " + juce::String ((s % 3600) / 60) + " min" : juce::String (juce::jmax (1, s / 60)) + " min"; }
    static juce::String fmtBytes (juce::int64 b) { return b >= (juce::int64) 1 << 30 ? juce::String (b / 1073741824.0, 1) + " GB" : juce::String ((int) (b / 1048576)) + " MB"; }

    int lastEstimateSec = 0;

    void updateInfo()
    {
        int done = 0, exi = 0, seconds = 0;
        for (auto& n : needed)
        {
            if (isDone (n)) { ++done; continue; }
            if (n.exi || n.external) ++exi;
            if (n.drum)
                seconds += (int) ((highNote.getValue() - lowNote.getValue() + 1) * (0.7 + 0.5 * (int) exiVelocities (n).size() * (hold.getValue() * 0.4 + 1.0)));
            else if (! n.exi && ! n.external && zoneToggle.getToggleState())
                seconds += (int) ((highNote.getValue() - lowNote.getValue() + 1) * 1.1 + 12 * (hold.getValue() + release.getValue() * 0.6 + 0.4));   // ~12 zona
            else
                seconds += estimateSecondsPerMs() * (n.exi || n.external ? (n.analyzed && n.anVelFlat ? 1 : (int) exiVelocities (n).size()) : 1);
        }
        lastEstimateSec = seconds;
        info.setText (juce::String ((int) needed.size() - exi) + " MS + " + juce::String (exi) + " EXi/other, done " + juce::String (done)
                      + juce::String::fromUTF8 ("\nleft: ~") + fmtTime (seconds) + ", ~" + fmtBytes (estimateBytes (seconds)) + " disk",
                      juce::dontSendNotification);
    }

    int estimateSecondsPerMs() const
    {
        const int notes = (int) ((highNote.getValue() - lowNote.getValue()) / step.getValue()) + 1;
        return (int) (notes * (hold.getValue() + release.getValue() * 0.6 + 0.4));
    }

    // program za jedan multisempl: OSC1 MS1 = taj multisempl, sve ostalo neutralno, bez efekata
    static void patchProgram (juce::uint8* p, const NeededMs& n)
    {
        juce::String name = "AS " + n.label();
        std::fill (p, p + 24, 0);
        for (int k = 0; k < juce::jmin (24, name.length()); ++k) p[k] = (juce::uint8) name[k];
        p[2558] = 0;            // Single, poly
        p[2565] = 0;            // Bus L/R
        p[2580] |= 0x80;        // Track EQ Bypass: multisempl se snima suv, EQ programa daje Workstation
        p[2684] = 0; p[2685] = 127;
        for (int z = 0; z < 8; ++z)
        {
            auto* zp = p + Kronos::osc1ZonesOffset + z * Kronos::zoneSize;
            if (z == 0) { std::copy (n.zone.begin(), n.zone.end(), zp); zp[20] = 127; zp[21] = 0; }
            else zp[0] = 0;
        }
        for (int z = 0; z < 7; ++z) { p[2950 + z * 3] = 1; p[2951 + z * 3] = 0; p[2952 + z * 3] = 0; }
        p[2972] = 1; p[2973] = 127;
        p[3012] = 0; p[3013] = 0; p[3014] = 0; p[3015] = 0;      // oktava, transpose, tune
        p[3019] = 0; p[3022] = 0; p[3030] = 0; p[3024] = 0;      // pitch EG, LFO, JS+Y, portamento
        p[3036] = 0; p[3047] = 99; p[3048] = 0; p[3056] = 0; p[3057] = 0;   // filter otvoren
        p[3141] = 127; p[3142] = 0;                                // jacina, bez velocity uticaja
        const juce::uint8 amp[8] = { 0, 0, 99, 0, 99, 0, 99, 20 };
        std::copy (amp, amp + 8, p + 3151);
        p[3193] = 64; p[3196] = 0; p[3197] = 0;                    // pan centar, send MFX 0
    }

    //==========================================================================
    //  SysEx: menja se samo trenutni program na Kronosu (edit buffer), nista se ne snima u memoriju.
    //  Format: F0 42 3g 68 <funkcija> ... F7, podaci 7-bitno pakovani (7 bajtova -> 8).
    //==========================================================================
    static std::vector<juce::uint8> unpack7 (const juce::uint8* m, int len)
    {
        std::vector<juce::uint8> out;
        while (len > 0)
        {
            const int chunk = juce::jmin (8, len);
            for (int i = 0; i < chunk - 1; ++i)
                out.push_back ((juce::uint8) (m[i + 1] | ((m[0] & (1 << i)) ? 0x80 : 0)));
            m += chunk;
            len -= chunk;
        }
        return out;
    }

    static std::vector<juce::uint8> pack7 (const std::vector<juce::uint8>& in)
    {
        std::vector<juce::uint8> out;
        for (size_t i = 0; i < in.size(); i += 7)
        {
            const size_t chunk = juce::jmin ((size_t) 7, in.size() - i);
            juce::uint8 hi = 0;
            for (size_t k = 0; k < chunk; ++k) if (in[i + k] & 0x80) hi |= (juce::uint8) (1 << k);
            out.push_back (hi);
            for (size_t k = 0; k < chunk; ++k) out.push_back (in[i + k] & 0x7f);
        }
        return out;
    }

    const juce::uint8* rawProgram (const Kronos::Program& p) const
    {
        for (auto& b : banks)
            if (b.bankId == p.bankId && b.exi == p.exi && p.number < b.count)
                return (const juce::uint8*) pcgData.getData() + b.firstItem + (size_t) p.number * Kronos::programSize;
        return nullptr;
    }

    static constexpr int hd1DumpSize = 3706;   // Prog_HD-1.txt (Korg MIDI implementation)

    juce::uint8 sysexChannelByte() const { return (juce::uint8) (0x30 | ((int) channel.getValue() - 1)); }

    void handleIncomingSysex (const juce::MidiMessage& m)
    {
        if (! m.isSysEx()) return;
        const auto* d = m.getSysExData();
        const int n = m.getSysExDataSize();
        if (n < 4 || d[0] != 0x42 || d[2] != 0x68) return;
        const juce::ScopedLock sl (sysexLock);
        lastSysex.assign (d, d + n);     // bez F0/F7
        sysexEvent.signal();
    }

    // posalji SysEx (bez F0/F7) i sacekaj odgovor sa datom funkcijom
    std::optional<std::vector<juce::uint8>> sysexCall (const std::vector<juce::uint8>& body, juce::uint8 replyFunc, int timeoutMs)
    {
        {
            const juce::ScopedLock sl (sysexLock);
            lastSysex.clear();
        }
        sysexEvent.reset();
        if (midi == nullptr) return std::nullopt;
        midi->sendMessageNow (juce::MidiMessage::createSysExMessage (body.data(), (int) body.size()));
        const auto t0 = juce::Time::getMillisecondCounter();
        while (juce::Time::getMillisecondCounter() - t0 < (juce::uint32) timeoutMs)
        {
            sysexEvent.wait (50);
            const juce::ScopedLock sl (sysexLock);
            if (lastSysex.size() > 3 && lastSysex[3] == replyFunc)
                return lastSysex;
        }
        return std::nullopt;
    }

    // procitaj trenutni program sa Kronosa i upamti ga kao sablon
    void readTemplateFromKronos()
    {
        if (midiIn == nullptr || midi == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SysEx",
                juce::String::fromUTF8 ("Choose the MIDI input and output of the Kronos."));
            return;
        }
        sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);   // Kronos u PROGRAM mod
        // lista za semplovanje odredjuje banku: ROM -> Init Program (HD-1, npr. USER-FF 006), EXi -> EXi program
        const NeededMs* firstRom = nullptr;
        const NeededMs* firstExi = nullptr;
        bool firstIsExi = false;
        for (auto& n : needed)
        {
            if (n.external || n.drum) continue;
            if (firstRom == nullptr && firstExi == nullptr) firstIsExi = n.exi;
            if (n.exi) { if (firstExi == nullptr) firstExi = &n; }
            else if (firstRom == nullptr) firstRom = &n;
        }
        juce::MouseCursor::showWaitCursor();
        if (firstRom != nullptr)
        {
            const auto* init = initHd1Program();
            setStatus (juce::String::fromUTF8 ("Switching the Kronos to ") + (init != nullptr ? init->id() + " " + init->name : juce::String ("an HD-1 program")) + "...");
            if (init != nullptr && ! selectKronosProgram (init->bankId, init->number, init->name, false))
            {
                juce::MouseCursor::hideWaitCursor();
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SysEx",
                    juce::String::fromUTF8 ("The Kronos did not switch to ") + init->id() + " " + init->name
                    + juce::String::fromUTF8 (". Check GLOBAL > MIDI: Program Change and Bank Change receive on, and that the loaded PCG is the same as on the Kronos. Or select it manually and click again."));
                return;
            }
        }
        else if (firstExi != nullptr)
        {
            juce::String err;
            const bool ok = ensureKronosKind (*firstExi, &err);
            juce::MouseCursor::hideWaitCursor();
            if (! ok) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "EXi", err); return; }
            templateInfo.setText (juce::String::fromUTF8 ("Kronos is on an EXi program. The list has only EXi programs: no template needed"), juce::dontSendNotification);
            setStatus (juce::String::fromUTF8 ("Kronos switched to EXi (") + firstExi->programId + ")");
            return;
        }
        auto reply = sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x74, 0x00 }, 0x75, 3000);
        juce::MouseCursor::hideWaitCursor();
        if (! reply)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SysEx",
                juce::String::fromUTF8 ("The Kronos did not answer. Check the MIDI channel (Kronos Global MIDI channel), that the Kronos is in PROGRAM mode and that GLOBAL > MIDI has Exclusive receive/send on."));
            return;
        }
        // Korg: 42 3g 68 75 obj version data... (podaci od bajta 6, 7-bitno pakovani);
        // HD-1 program ima 3706 bajtova, raspored isti kao pocetak programa u PCG-u
        const auto& r = *reply;
        const int best = 6, bestShift = 0;
        auto dec = unpack7 (r.data() + best, (int) r.size() - best);
        if (r.size() < 8 || r[4] != 0x00 || (int) dec.size() < hd1DumpSize)
        {
            outFolder.createDirectory();
            outFolder.getChildFile ("Kronos-program.syx").replaceWithData (r.data(), r.size());
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SysEx",
                juce::String::fromUTF8 ("The current program is not an HD-1 program (") + juce::String ((int) dec.size())
                + juce::String::fromUTF8 (" bytes). On the Kronos select Init Program (HD-1, e.g. USER-FF 006) and try again."));
            return;
        }
        sxStart = best;
        sxShift = bestShift;
        sxHeader.assign (r.begin(), r.begin() + best);
        sxHeader[3] = 0x75;
        sxTemplate = unpack7 (r.data() + best, (int) r.size() - best);
        juce::String nm;
        for (int k = 0; k < 24 && sxTemplate[(size_t) (sxShift + k)] != 0; ++k) nm << (juce::juce_wchar) sxTemplate[(size_t) (sxShift + k)];
        templateInfo.setText (juce::String::fromUTF8 ("Template from Kronos: ") + nm.trim() + " (" + juce::String ((int) sxTemplate.size()) + juce::String::fromUTF8 (" bytes)"),
                              juce::dontSendNotification);
        outFolder.createDirectory();
        outFolder.getChildFile ("Kronos-program.syx").replaceWithData (r.data(), r.size());
        lastKind = 1;
        // prvi na listi je EXi: posle citanja sablona vrati Kronos na EXi program
        if (firstIsExi && firstExi != nullptr)
        {
            juce::String err;
            if (! ensureKronosKind (*firstExi, &err))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "EXi", err);
        }
        setStatus (juce::String::fromUTF8 ("Template read: ") + nm.trim());
    }

    // posalji izmenjen program u edit buffer Kronosa
    bool sendProgramSysex (const NeededMs& n)
    {
        if (sxTemplate.empty()) return false;
        auto data = sxTemplate;
        patchProgram (data.data() + sxShift, n);
        auto body = sxHeader;
        auto packed = pack7 (data);
        body.insert (body.end(), packed.begin(), packed.end());
        auto reply = sysexCall (body, 0x24, 2000);
        if (reply && reply->size() > 5 && (*reply)[5] != 0)
        {
            // npr. bank conflict: izaberi HD-1 program pa posalji ponovo
            lastKind = 0;
            NeededMs hd; ensureKronosKind (hd);
            reply = sysexCall (body, 0x24, 2000);
            if (reply && reply->size() > 5 && (*reply)[5] != 0)
                setStatus (juce::String::fromUTF8 ("Kronos reports error ") + juce::String::toHexString ((int) (*reply)[5]));
        }
        return true;
    }

    // jacine za EXi program (velocity slojevi)
    std::vector<int> exiVelocities (const NeededMs& n) const
    {
        int layers = n.external ? n.layers : exiVelBox.getSelectedId() - 100;
        if (layers <= 0 && n.drum) layers = 3;
        if (layers <= 0)
        {
            auto has = [&n] (int a) { return n.alg[0] == a || n.alg[1] == a; };
            layers = has (8) ? 6 : has (9) ? 5 : has (4) ? 4 : (n.alg[0] == 3 && (n.alg[1] == 0 || n.alg[1] == 3)) ? 1 : 2;
        }
        std::vector<int> v;
        for (int i = 1; i <= layers; ++i) v.push_back (juce::jlimit (1, 127, (int) std::round (127.0 * i / layers)));
        return v;
    }

    // EXi program bez efekata (Prog_EXi_Common.txt): IFX 1-12, MFX, TFX iskljuceni, Bus L/R,
    // sendovi 0, Drum Track i Hold iskljuceni. Sve ostalo (sinteza, envelope, filter) ostaje kako jeste.
    static void makeExiDry (juce::uint8* p, bool keepIfx)
    {
        if (! keepIfx)
        {
            for (int i = 0; i < 12; ++i) p[89 + 74 * i] &= (juce::uint8) ~0x40;
            p[2565] &= (juce::uint8) ~0x1f;                 // Bus Select L/R
        }
        else
        {
            // IFX ostaju; zvuk koji ide u IFX izlazi na L/R (bus IFX-a), a ne u sub izlaze
            for (int i = 0; i < 12; ++i) p[93 + 74 * i] &= (juce::uint8) ~0x0f;   // izlaz IFX-a: L/R (ne sub izlazi)
            if ((p[2565] & 0x1f) > 12) p[2565] &= (juce::uint8) ~0x1f;          // program na sub izlazu -> L/R
        }
        for (int o : { 977, 1045, 1117, 1185 }) p[o] &= (juce::uint8) ~0x40;   // MFX1, MFX2, TFX1, TFX2
        p[1048] = 0; p[1049] = 0;                           // MFX return
        p[2864] = 0; p[2865] = 0; p[3916] = 0; p[3917] = 0; // EXi1 / EXi2 send MFX
        p[1295] &= (juce::uint8) ~0x10;                     // Drum Track Off
        p[2558] &= (juce::uint8) ~0x40;                     // Hold Off
    }

    // zaglavlje za Current Object Dump (verzija programa) - uzima se iz odgovora Kronosa
    bool ensureSysexHeader (juce::String& err)
    {
        if (! sxHeader.empty()) return true;
        if (midiIn == nullptr || midi == nullptr) { err = juce::String::fromUTF8 ("For EXi programs choose the MIDI input and output of the Kronos."); return false; }
        sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);
        auto r = sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x74, 0x00 }, 0x75, 3000);
        if (! r || r->size() < 8)
        {
            err = juce::String::fromUTF8 ("The Kronos did not answer the SysEx. Check the MIDI channel, PROGRAM mode and GLOBAL > MIDI > Exclusive.");
            return false;
        }
        sxHeader.assign (r->begin(), r->begin() + 6);
        sxHeader[3] = 0x75;
        return true;
    }

    bool sendExiSysex (const NeededMs& n, juce::String& err)
    {
        if (! ensureSysexHeader (err)) return false;
        auto data = n.program;
        makeExiDry (data.data(), exiIfxToggle.getToggleState());
        auto body = sxHeader;
        body[1] = sysexChannelByte();
        body[4] = 0x00;
        auto packed = pack7 (data);
        body.insert (body.end(), packed.begin(), packed.end());
        sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);   // PROGRAM mod
        auto reply = sysexCall (body, 0x24, 3000);
        if (reply && reply->size() > 5 && (*reply)[5] != 0)
        {
            lastKind = 0;
            ensureKronosKind (n);
            reply = sysexCall (body, 0x24, 3000);
        }
        if (reply && reply->size() > 5 && (*reply)[5] != 0)
        {
            err = juce::String::fromUTF8 ("Kronos reports error ") + juce::String::toHexString ((int) (*reply)[5]) + juce::String::fromUTF8 (" for EXi program (") + n.programId + ")";
            return false;
        }
        return true;
    }

    //==========================================================================
    void makePcg()
    {
        if (pcg == nullptr || needed.empty()) return;
        const int tIdx = templateBox.getSelectedId() - 1;
        const juce::uint32 bankId = (juce::uint32) (bankBox.getSelectedId() - 1);
        const RawBank* target = nullptr;
        for (auto& b : banks) if (b.bankId == bankId && ! b.exi) target = &b;
        if (tIdx < 0 || target == nullptr) return;

        // sablon: sirovi bajtovi izabranog programa
        const auto& tp = pcg->programs[(size_t) tIdx];
        const RawBank* tb = nullptr;
        for (auto& b : banks) if (b.bankId == tp.bankId && ! b.exi) tb = &b;
        if (tb == nullptr) return;
        auto* data = (juce::uint8*) pcgData.getData();
        std::vector<juce::uint8> tpl (data + tb->firstItem + (size_t) tp.number * Kronos::programSize,
                                      data + tb->firstItem + (size_t) (tp.number + 1) * Kronos::programSize);

        juce::MemoryBlock out (pcgData);
        auto* o = (juce::uint8*) out.getData();
        int slot = (int) startNum.getValue();
        int written = 0;
        juce::StringArray mapLines;
        for (auto& n : needed)
        {
            if (n.exi || n.external) continue;    // EXi ide samo preko SysEx-a, druga klavijatura preko Program Change
            if (slot >= target->count) { n.slot = -1; continue; }
            auto prg = tpl;
            patchProgram (prg.data(), n);

            std::copy (prg.begin(), prg.end(), o + target->firstItem + (size_t) slot * Kronos::programSize);
            n.slot = slot;
            mapLines.add (juce::String (slot) + " " + n.romKey + " " + n.usedBy);
            ++slot;
            ++written;
        }
        const auto outFile = pcgFile.getSiblingFile (pcgFile.getFileNameWithoutExtension() + "_AUTOSAMPLER.PCG");
        if (! outFile.replaceWithData (out.getData(), out.getSize()))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "PCG", juce::String::fromUTF8 ("Cannot write ") + outFile.getFullPathName());
            return;
        }
        outFile.withFileExtension ("txt").replaceWithText (mapLines.joinIntoString ("\n"));
        neededList.repaint();
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "PCG",
            juce::String::fromUTF8 ("Created ") + outFile.getFileName() + ": " + juce::String (written) + " programs in bank "
            + Kronos::programBankName (bankId) + juce::String::fromUTF8 (" from number ") + juce::String ((int) startNum.getValue())
            + (written < (int) needed.size() ? juce::String::fromUTF8 ("\nNot all fit in the bank: after recording create the next PCG for the rest.") : juce::String())
            + juce::String::fromUTF8 ("\n\nOn the Kronos: Disk → Load → choose this PCG and load only bank ") + Kronos::programBankName (bankId) + ".");
    }

    //==========================================================================
    void showAudioSettings()
    {
        auto* sel = new juce::AudioDeviceSelectorComponent (deviceManager, 1, 2, 0, 2, false, false, true, false);
        sel->setSize (520, 380);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (sel);
        o.dialogTitle = "Audio input";
        o.dialogBackgroundColour = panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void refreshMidi()
    {
        midiDevices = juce::MidiOutput::getAvailableDevices();
        midiOut.clear();
        int kronos = -1;
        for (int i = 0; i < midiDevices.size(); ++i)
        {
            midiOut.addItem (midiDevices[i].name, i + 1);
            if (kronos < 0 && midiDevices[i].name.containsIgnoreCase ("kronos")) kronos = i;
        }
        if (kronos >= 0) midiOut.setSelectedId (kronos + 1, juce::dontSendNotification);
        openMidi();
    }

    void openMidi()
    {
        const int i = midiOut.getSelectedId() - 1;
        midi.reset();
        if (i >= 0 && i < midiDevices.size())
            midi = juce::MidiOutput::openDevice (midiDevices[i].identifier);
    }

    void openMidiIn()
    {
        if (midiIn != nullptr) midiIn->stop();
        midiIn.reset();
        const int i = midiInBox.getSelectedId() - 1;
        if (i >= 0 && i < midiInDevices.size())
        {
            midiIn = juce::MidiInput::openDevice (midiInDevices[i].identifier, &inCallback);
            if (midiIn != nullptr) midiIn->start();
        }
    }

    void send (const juce::MidiMessage& m) { if (midi != nullptr) midi->sendMessageNow (m); }

    void selectProgram (int number)
    {
        const int ch = (int) channel.getValue();
        send (juce::MidiMessage::controllerEvent (ch, 0, (int) cc0.getValue()));
        send (juce::MidiMessage::controllerEvent (ch, 32, (int) cc32.getValue()));
        send (juce::MidiMessage::programChange (ch, number));
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Sample folder", outFolder);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult() != juce::File())
                                  {
                                      outFolder = fc.getResult();
                                      folderLabel.setText (outFolder.getFullPathName(), juce::dontSendNotification);
                                  }
                              });
    }

    juce::File msFolder (const NeededMs& n) const
    {
        if (n.external)
            return outFolder.getChildFile ("Druge klavijature").getChildFile (juce::File::createLegalFileName (n.extName));
        // ROM / EXs / WaveSeq multisempl: folder se zove samo kao na Kronosu (npr. "ROM 0155 Musette"),
        // bez imena programa: isti sempl koristi vise usera. EXi i drum kit su program, pa nose i ime programa.
        auto nm = (n.exi || n.drum) ? n.label() + " " + n.usedBy : n.label();
        const auto byName = outFolder.getChildFile (juce::File::createLegalFileName (nm.trim()));
        if (byName.isDirectory() || n.romKey.isEmpty()) return byName;
        // isti zvuk (isti kljuc) vec postoji u drugom folderu (npr. dodat rucno ili iz drugog programa):
        // snima se preko njega, da se ne prave duplikati (24-bit + 16-bit)
        const auto existing = folderForKey (n.romKey);
        return existing != juce::File() ? existing : byName;
    }

    // kronos-key.txt -> folder (kes, osvezava se na par sekundi)
    juce::File folderForKey (const juce::String& key) const
    {
        // zove se i iz niti za snimanje i iz prozora (lista, info): bez zakljucavanja dve niti istovremeno
        // menjaju isti std::map, sto kvari memoriju i program nestane bez poruke o gresci
        const juce::ScopedLock sl (keyCacheLock);
        const auto now = juce::Time::getMillisecondCounter();
        if (keyCacheFolder != outFolder || keyCacheTime.load() == 0 || now - keyCacheTime.load() > 3000)
        {
            keyCache.clear();
            for (auto& d : outFolder.findChildFiles (juce::File::findDirectories, false))
            {
                const auto kf = d.getChildFile ("kronos-key.txt");
                if (! kf.existsAsFile()) continue;
                const auto k = kf.loadFileAsString().trim().toLowerCase();
                auto it = keyCache.find (k);
                // vise foldera za isti kljuc: prednost ima originalni (bez " manual") sa semplovima
                const bool better = it == keyCache.end()
                    || (it->second.getFileName().endsWith (" manual") && ! d.getFileName().endsWith (" manual"));
                if (better) keyCache[k] = d;
            }
            keyCacheFolder = outFolder;
            keyCacheTime = juce::jmax ((juce::uint32) 1, now);
        }
        auto it = keyCache.find (key.trim().toLowerCase());
        return it != keyCache.end() ? it->second : juce::File();
    }
    mutable std::map<juce::String, juce::File> keyCache;
    mutable juce::File keyCacheFolder;
    mutable std::atomic<juce::uint32> keyCacheTime { 0 };   // 0 = napravi kes ponovo
    juce::CriticalSection keyCacheLock;

    // Spoji duplikate: isti zvuk snimljen u dva foldera (stari 24-bit i novi "... manual").
    // Ostaje ime starog foldera (na njega se pozivaju useri u Workstation-u), a u njega idu najnoviji semplovi.
    // Stari semplovi idu u Recycle Bin.
    int mergeDuplicateFolders (juce::StringArray& log)
    {
        std::map<juce::String, juce::Array<juce::File>> groups;
        for (auto& d : outFolder.findChildFiles (juce::File::findDirectories, false))
        {
            const auto kf = d.getChildFile ("kronos-key.txt");
            if (kf.existsAsFile()) groups[kf.loadFileAsString().trim().toLowerCase()].add (d);
        }
        auto stamp = [] (const juce::File& d)
        {
            const auto t = d.getChildFile ("kronos-ms.txt");
            return t.existsAsFile() ? t.getLastModificationTime().toMilliseconds() : (juce::int64) 0;
        };
        auto wavs = [] (const juce::File& d) { return d.getNumberOfChildFiles (juce::File::findFiles, "*.wav"); };
        int merged = 0;
        for (auto& [key, dirs] : groups)
        {
            if (dirs.size() < 2) continue;
            juce::File target, source;
            for (auto& d : dirs) if (! d.getFileName().endsWith (" manual") && (target == juce::File() || wavs (d) > wavs (target))) target = d;
            if (target == juce::File()) target = dirs[0];
            for (auto& d : dirs) if (wavs (d) > 0 && (source == juce::File() || stamp (d) > stamp (source))) source = d;
            if (source == juce::File()) continue;
            if (source != target)
            {
                for (auto& f : target.findChildFiles (juce::File::findFiles, false))
                    if (! f.moveToTrash()) f.deleteFile();
                for (auto& f : source.findChildFiles (juce::File::findFiles, false))
                    f.moveFileTo (target.getChildFile (f.getFileName()));
            }
            for (auto& d : dirs)
                if (d != target && ! d.moveToTrash()) d.deleteRecursively();
            log.add (target.getFileName() + "  <-  " + source.getFileName());
            ++merged;
        }
        keyCacheTime = 0;
        return merged;
    }

    // gotovo samo ako je snimljen bas ovaj program (EXi program izmenjen na Kronosu ima drugi kljuc)
    bool isDone (const NeededMs& n) const
    {
        const auto tag = msFolder (n).getChildFile ("kronos-ms.txt");
        if (! tag.existsAsFile()) return false;
        if (! juce::StringArray::fromLines (tag.loadFileAsString())[0].trim().equalsIgnoreCase (n.romKey)) return false;
        // prazan ili nedovrsen folder (nema WAV-a, ili ROM bez mape) nije gotov: snima se ponovo
        const auto folder = msFolder (n);
        if (folder.getNumberOfChildFiles (juce::File::findFiles, "*.wav") == 0) return false;
        if (! n.exi && ! n.external && ! n.drum && folder.getChildFile ("kronos-zones.txt").existsAsFile()
            && ! folder.getChildFile ("LiveSampler-mapa.xml").existsAsFile()) return false;
        return true;
    }

    // Sredjivanje foldera ROM semplova: jedan folder po ROM multisemplu, ime kao na Kronosu.
    // Vise foldera istog sempla (stari 24-bit + novi 16-bit, "... manual", "... ime programa"):
    // ostaje najnovije snimljen (sa semplovima), ostali idu u Recycle Bin. Stara imena idu u aliases.txt,
    // pa Pikaso Workstation sam prebaci usere na novo ime (posle Refresh). Vraca broj promena.
    int tidyRomFolders()
    {
        if (outFolder == juce::File() || ! outFolder.isDirectory() || isThreadRunning()) return 0;
        struct F { juce::File dir; juce::String label; juce::int64 time = 0; int wavs = 0; };
        std::map<juce::String, std::vector<F>> groups;
        for (auto& d : outFolder.findChildFiles (juce::File::findDirectories, false))
        {
            const auto tag = d.getChildFile ("kronos-ms.txt");
            const auto kf = d.getChildFile ("kronos-key.txt");
            juce::String key, label;
            if (tag.existsAsFile())
            {
                const auto lines = juce::StringArray::fromLines (tag.loadFileAsString());
                key = lines[0].trim().toLowerCase();
                label = lines[1].trim();
            }
            else if (kf.existsAsFile()) key = kf.loadFileAsString().trim().toLowerCase();
            if (! key.startsWith ("rom:")) continue;                  // EXi / drum kit / druge klavijature ostaju kako jesu
            key = key.substring (6);                                   // bez prvog bajta (MS Type + zastavice): isti ROM sempl
            F f { d, label, tag.existsAsFile() ? tag.getLastModificationTime().toMilliseconds() : 0,
                  d.getNumberOfChildFiles (juce::File::findFiles, "*.wav") };
            groups[key].push_back (f);
        }
        int changes = 0;
        auto trash = [&changes] (const juce::File& d) { if (! d.moveToTrash()) d.deleteRecursively(); ++changes; };
        for (auto& [key, list] : groups)
        {
            const F* best = nullptr;
            for (auto& f : list)
                if (f.wavs > 0 && f.label.isNotEmpty() && (best == nullptr || f.time > best->time)) best = &f;
            if (best == nullptr) continue;   // nijedan nije snimljen do kraja: ne dira se (snima se ponovo)
            const auto target = outFolder.getChildFile (juce::File::createLegalFileName (best->label));
            juce::StringArray oldNames;
            for (auto& f : list)
                if (&f != best)
                {
                    if (f.dir != target) oldNames.addIfNotAlreadyThere (f.dir.getFileName());
                    trash (f.dir);
                }
            auto dir = best->dir;
            if (dir != target && ! target.exists())
            {
                const auto oldName = dir.getFileName();
                if (dir.moveFileTo (target)) { oldNames.addIfNotAlreadyThere (oldName); dir = target; ++changes; }
            }
            if (! oldNames.isEmpty())
            {
                const auto al = dir.getChildFile ("aliases.txt");
                juce::StringArray all;
                if (al.existsAsFile()) all = juce::StringArray::fromLines (al.loadFileAsString());
                for (auto& o : oldNames) all.addIfNotAlreadyThere (o);
                all.removeString (dir.getFileName());
                all.removeEmptyStrings();
                al.replaceWithText (all.joinIntoString ("\n"));
            }
        }
        keyCacheTime = 0;   // kes foldera po kljucu se pravi ponovo
        return changes;
    }

    // pre snimanja: ako u folderu stoje semplovi nekog drugog (starijeg) programa, brisu se
    void prepareFolder (const NeededMs& n)
    {
        const auto folder = msFolder (n);
        const auto keyFile = folder.getChildFile ("kronos-key.txt");
        const auto tag = folder.getChildFile ("kronos-ms.txt");
        juce::String old;
        if (keyFile.existsAsFile()) old = keyFile.loadFileAsString().trim();
        else if (tag.existsAsFile()) old = juce::StringArray::fromLines (tag.loadFileAsString())[0].trim();
        if (folder.isDirectory() && old.isNotEmpty() && ! old.equalsIgnoreCase (n.romKey))
            folder.deleteRecursively();
        folder.createDirectory();
        keyFile.replaceWithText (n.romKey);
    }

    //==========================================================================
    void startSampling()
    {
        if (midi == nullptr || deviceManager.getCurrentAudioDevice() == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Choose the MIDI output to the Kronos and the audio input (Kronos USB)."));
            return;
        }
        if (outFolder == juce::File())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Choose the sample folder (button \"Sample folder...\"). Keep it inside a Pikaso Workstation library folder, e.g. C:\\KRONOS\\KRONOS ROM."));
            return;
        }
        tidyRomFolders();
        useSysex = sysexMode.getToggleState();
        bool anyRom = false, anyExi = false;
        for (auto& n : needed) { anyRom = anyRom || (! n.exi && ! n.external && ! isDone (n)); anyExi = anyExi || (n.exi && ! isDone (n)); }
        if (anyExi && midiIn == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("EXi programs are sent via SysEx: choose the MIDI input from the Kronos too."));
            return;
        }
        if (useSysex && anyRom && sxTemplate.empty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("On the Kronos select Init Program (e.g. USER-FF 006), then click \"Read template from Kronos\"."));
            return;
        }
        bool anySlot = useSysex || ! anyRom;
        for (auto& n : needed) anySlot = anySlot || n.slot >= 0;
        if (! anySlot)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("First create the PCG (step 2) and load it on the Kronos."));
            return;
        }
        settings.channel = (int) channel.getValue();
        settings.low = (int) lowNote.getValue();
        settings.high = (int) highNote.getValue();
        settings.step = (int) step.getValue();
        settings.velocity = (int) velocity.getValue();
        settings.hold = hold.getValue();
        settings.release = release.getValue();
        settings.autoLoop = autoLoopToggle.getToggleState();
        settings.xfadeMs = xfadeMs.getValue();
        settings.minLoopMs = minLoopMs.getValue();
        settings.zones = zoneToggle.getToggleState();
        {
            const juce::ScopedLock sl (jobsLock);
            exiVels.clear();
            for (auto& n : needed) exiVels.push_back (n.exi || n.external || n.drum ? exiVelocities (n) : std::vector<int> {});
        }
        outFolder.createDirectory();
        saveSettings();
        updateInfo();
        const auto need = estimateBytes (lastEstimateSec);
        const auto freeB = outFolder.getBytesFreeOnVolume();
        juce::String msg = juce::String::fromUTF8 ("Estimate: ~") + fmtTime (lastEstimateSec) + ", ~" + fmtBytes (need) + (bits16Toggle.getToggleState() ? " (16-bit)" : " (24-bit)")
                         + "\n" + juce::String::fromUTF8 ("Free on the disk: ") + fmtBytes (freeB)
                         + (freeB > 0 && freeB < need + ((juce::int64) 1 << 30) ? juce::String::fromUTF8 ("\n\nWARNING: probably NOT enough disk space. Free some space or choose 16-bit.") : juce::String())
                         + juce::String::fromUTF8 ("\n\nYou can add more sounds to the list while it records.");
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Autosampler", msg, "START", "Cancel", nullptr,
            juce::ModalCallbackFunction::create ([safe] (int r)
            {
                if (safe == nullptr || r == 0 || safe->isThreadRunning()) return;
                safe->analyzeOnly = false;
                safe->autoAnalyze = safe->autoAnalyzeToggle.getToggleState();
                safe->bitsPerSample = safe->bits16Toggle.getToggleState() ? 16 : 24;
                safe->report.clear();
                safe->startButton.setButtonText ("STOP");
                safe->startThread();
            }));
    }

    // procena izabranih zvukova (ili svih ako nista nije izabrano)
    void startAnalysis()
    {
        if (isThreadRunning() || needed.empty()) return;
        if (midi == nullptr || deviceManager.getCurrentAudioDevice() == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Choose the MIDI output and the audio input."));
            return;
        }
        analyzeRows.clear();
        auto sel = neededList.getSelectedRows();
        for (int k = 0; k < sel.size(); ++k) analyzeRows.push_back (sel[k]);
        if (analyzeRows.empty()) for (int k = 0; k < (int) needed.size(); ++k) analyzeRows.push_back (k);
        bool needTemplate = false;
        for (int r : analyzeRows) needTemplate = needTemplate || (! needed[(size_t) r].exi && ! needed[(size_t) r].external);
        useSysex = sysexMode.getToggleState();
        if (needTemplate && useSysex && sxTemplate.empty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("For ROM multisamples first click \"Read template from Kronos\"."));
            return;
        }
        settings.channel = (int) channel.getValue();
        settings.low = (int) lowNote.getValue();
        settings.high = (int) highNote.getValue();
        analyzeOnly = true;
        startButton.setButtonText ("STOP");
        startThread();
    }

    struct Settings { int channel = 1, low = 24, high = 108, step = 3, velocity = 127; double hold = 6, release = 3;
                      bool autoLoop = true, zones = true; double xfadeMs = 80, minLoopMs = 300; } settings;

    void setStatus (const juce::String& s)
    {
        const juce::ScopedLock sl (statusLock);
        statusText = s;
        // u log samo kad se promeni korak (ne svaka nota), da se vidi gde je stao ako se srusi
        const auto key = s.upToFirstOccurrenceOf ("  ", false, false).substring (0, 40);
        if (key != lastLoggedStatus) { lastLoggedStatus = key; juce::Logger::writeToLog ("status: " + s); }
    }
    juce::String lastLoggedStatus;

    // izabere zvuk za posao: druga klavijatura (Program Change), EXi (SysEx), ROM multisempl (SysEx ili PCG)
    // Kronos: edit buffer prima samo program istog tipa kao banka u kojoj je (EXi banka ili HD-1 banka),
    // inace javlja "bank conflict". Zato se pre slanja izabere program odgovarajuceg tipa (Program Change).
    int lastKind = 0;   // 0 nepoznato, 1 HD-1, 2 EXi

    // trenutni program na Kronosu: ime i velicina (HD-1 = 3706 bajtova, EXi = 4960)
    juce::String readCurrentProgram (int& size)
    {
        size = 0;
        auto r = sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x74, 0x00 }, 0x75, 2000);
        if (! r || r->size() < 8) return {};
        const auto dec = unpack7 (r->data() + 6, (int) r->size() - 6);
        size = (int) dec.size();
        juce::String nm;
        for (int k = 0; k < 24 && k < (int) dec.size() && dec[(size_t) k] != 0; ++k) nm << (juce::juce_wchar) dec[(size_t) k];
        return nm.trim();
    }

    int userBankBase = -1;   // CC32 za USER-A (po Kronos MIDI implementaciji 8; 6/7 samo kao rezerva)
    int bankMsb = -1;        // CC0: 00 kad je Global Bank Map KORG, 3F kad je GM(2)

    // izaberi program na Kronosu (Bank Select + Program Change) i proveri da je stvarno izabran (ime i vrsta)
    bool selectKronosProgram (juce::uint32 bankId, int number, const juce::String& expectedName, bool wantExi)
    {
        const int hi = (int) (bankId >> 16), lo = (int) (bankId & 0xffff);
        std::vector<std::pair<int, int>> tries;   // (CC0, CC32)
        auto addLsb = [&] (int lsb)
        {
            for (int msb : { bankMsb >= 0 ? bankMsb : 0, bankMsb >= 0 ? (bankMsb == 0 ? 0x3F : 0) : 0x3F })
                if (std::find (tries.begin(), tries.end(), std::make_pair (msb, lsb)) == tries.end())
                    tries.push_back ({ msb, lsb });
        };
        if (hi == 0 && lo <= 6) addLsb (lo);                     // INT-A..G = 00..06
        if (hi == 2)                                              // USER-A..G = 08..0E, USER-AA..GG = 0F..15
        {
            addLsb ((userBankBase >= 0 ? userBankBase : 8) + lo);
            for (int base : { 8, 6, 7 }) addLsb (base + lo);
        }
        if (tries.empty() || number < 0 || number > 127) return false;
        const int ch = settings.channel;
        sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);   // PROGRAM mod
        int size0 = 0;
        const auto origName = readCurrentProgram (size0);   // program pre prebacivanja
        if (size0 > 0 && (size0 >= 4900) == wantExi && expectedName.isNotEmpty() && origName.equalsIgnoreCase (expectedName.trim()))
            return true;   // vec je tu
        for (size_t k = 0; k < tries.size(); ++k)
        {
            const auto [msb, lsb] = tries[k];
            juce::Logger::writeToLog ("select program: CC0=" + juce::String (msb) + " CC32=" + juce::String (lsb) + " PC=" + juce::String (number)
                                      + "  (now: " + origName + ")");
            // Kronos ne ucitava ponovo program na kome vec stoji: ako je na tom mestu nas izmenjen program
            // ("AS ROM ..." u edit bufferu), PC na isti broj ne radi nista. Zato prvo susedni program, pa trazeni.
            send (juce::MidiMessage::controllerEvent (ch, 0, msb));
            send (juce::MidiMessage::controllerEvent (ch, 32, lsb));
            send (juce::MidiMessage::programChange (ch, number > 0 ? number - 1 : number + 1));
            wait (1500);
            send (juce::MidiMessage::controllerEvent (ch, 0, msb));
            send (juce::MidiMessage::controllerEvent (ch, 32, lsb));
            send (juce::MidiMessage::programChange (ch, number));
            // Kronos-u treba vremena da ucita program: prelaz EXi <-> HD-1 traje i nekoliko sekundi, a dok ucitava
            // i dalje javlja STARI program. Zato se ne odustaje dok stoji stari program (to nije znak greske),
            // nego tek kad se ustali neki treci program. Pita se na 1 s (svako pitanje Kronos prikaze kao "transmitting").
            juce::String lastName; int same = 0;
            for (int t = 0; t < 20 && ! threadShouldExit(); ++t)
            {
                wait (1000);
                int size = 0;
                const auto nm = readCurrentProgram (size);
                const bool isExi = size >= 4900;
                if (size > 0 && isExi == wantExi && (expectedName.isEmpty() || nm.equalsIgnoreCase (expectedName.trim())))
                {
                    if (hi == 2) userBankBase = lsb - lo;
                    bankMsb = msb;
                    juce::Logger::writeToLog ("select program: OK " + nm);
                    return true;
                }
                same = (size > 0 && nm == lastName) ? same + 1 : 0;
                lastName = nm;
                const bool stillOld = nm == origName;
                if (! stillOld && same >= 3) break;   // ustalio se neki drugi program: probaj sledeci Bank Select
                if (stillOld && t >= 14) break;       // ~15 s nista: Kronos ne prima ovaj Bank Select
            }
            juce::Logger::writeToLog ("select program: not reached, Kronos is on " + lastName);
        }
        return false;
    }

    // HD-1 Init Program iz PCG-a (npr. USER-FF 006), inace prvi HD-1 program
    const Kronos::Program* initHd1Program() const
    {
        if (pcg == nullptr) return nullptr;
        const Kronos::Program* pick = nullptr;
        for (auto& p : pcg->programs)
            if (p.hd1 && ! p.drumKit && (pick == nullptr || (p.name.startsWithIgnoreCase ("init") && ! pick->name.startsWithIgnoreCase ("init"))))
                pick = &p;
        return pick;
    }

    // Kronos prima program istog tipa kao banka u kojoj je; vraca false ako nije uspelo da prebaci
    bool ensureKronosKind (const NeededMs& n, juce::String* err = nullptr)
    {
        const int kind = n.exi ? 2 : 1;
        // uvek prvo proveri sta je stvarno izabrano na Kronosu (neko je mozda rucno promenio program),
        // da Kronos nikad ne dobije program pogresne vrste ("Program bank type conflict")
        {
            sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);   // PROGRAM mod
            int size = 0;
            readCurrentProgram (size);
            if (size > 0 && (size >= 4900) == n.exi) { lastKind = kind; return true; }
        }
        lastKind = 0;
        if (pcg == nullptr)
        {
            if (err != nullptr) *err = juce::String::fromUTF8 ("On the Kronos manually select an ") + (n.exi ? "EXi" : "HD-1") + " program (PCG not loaded).";
            return false;
        }
        bool ok = false;
        if (n.exi) ok = selectKronosProgram (n.progBank, n.progNum, n.usedBy, true);
        else
        {
            // HD-1 program: "Init..." iz PCG-a, inace prvi HD-1 program
            const auto* pick = initHd1Program();
            if (pick != nullptr) ok = selectKronosProgram (pick->bankId, pick->number, pick->name, false);
        }
        if (! ok)
        {
            // mozda je Kronos vec u pravoj vrsti banke
            int size = 0;
            readCurrentProgram (size);
            ok = size > 0 && (size >= 4900) == n.exi;
        }
        if (ok) lastKind = kind;
        else if (err != nullptr)
            *err = juce::String::fromUTF8 ("The Kronos did not switch to an ") + (n.exi ? "EXi" : "HD-1")
                 + juce::String::fromUTF8 (" program. Check on the Kronos GLOBAL > MIDI: Program Change and Bank Change receive on (filters enabled), "
                                           "and that the PCG is the same as on the Kronos. Or manually select ")
                 + (n.exi ? juce::String::fromUTF8 ("any EXi program (e.g. INT-A 000)") : juce::String::fromUTF8 ("Init Program (HD-1)")) + ".";
        return ok;
    }

    bool selectJob (const NeededMs& n, juce::String& err, bool& fatal)
    {
        fatal = false;
        if (n.drum)
        {
            // drum kit: pravi program sa Kronosa (sa njegovim drum kitom), bira se Program Change-om i proverava po imenu
            if (! selectKronosProgram (n.progBank, n.progNum, n.usedBy, false))
            {
                err = "The Kronos did not switch to the drum kit program " + n.programId + " " + n.usedBy
                      + ". Check GLOBAL > MIDI (Program / Bank Change receive) and that the PCG is the same as on the Kronos.";
                return false;
            }
            lastKind = 1;
            wait (1200);
            return true;
        }
        if (! n.external && ! ensureKronosKind (n, &err)) return false;
        if (n.external)
        {
            const int ch = settings.channel;
            if (n.msb >= 0) send (juce::MidiMessage::controllerEvent (ch, 0, n.msb));
            if (n.lsb >= 0) send (juce::MidiMessage::controllerEvent (ch, 32, n.lsb));
            if (n.pc >= 0) send (juce::MidiMessage::programChange (ch, n.pc));
            wait (1500);
            return true;
        }
        if (n.exi)
        {
            if (! sendExiSysex (n, err)) return false;
            wait (1500);
            return true;
        }
        if (useSysex)
        {
            if (! sendProgramSysex (n)) { err = juce::String::fromUTF8 ("Template not read"); fatal = true; return false; }
        }
        else
        {
            if (n.slot < 0) { err = "no program in the PCG"; return false; }
            selectProgram (n.slot);
        }
        wait (900);
        return true;
    }

    void run() override
    {
        crashLogThreadStart();
        try { runJobs(); }
        catch (const std::exception& e) { juce::Logger::writeToLog (juce::String ("CRASH (exception): ") + e.what()); setStatus (juce::String ("Error: ") + e.what()); }
        catch (...) { juce::Logger::writeToLog ("CRASH (unknown exception)"); setStatus ("Error (unknown exception)"); }
    }

    void runJobs()
    {
        const int total = (int) needed.size();
        lastKind = 0;
        if (analyzeOnly)
        {
            int ok = 0;
            juce::String lastErr;
            for (size_t k = 0; k < analyzeRows.size() && ! threadShouldExit(); ++k)
            {
                const int i = analyzeRows[k];
                NeededMs n;
                {
                    const juce::ScopedLock sl (jobsLock);
                    if (i < 0 || i >= (int) needed.size()) continue;
                    n = needed[(size_t) i];
                }
                juce::String err; bool fatal = false;
                setStatus ("Analysis " + juce::String ((int) k + 1) + "/" + juce::String ((int) analyzeRows.size()) + "  " + n.label());
                if (! selectJob (n, err, fatal))
                {
                    n.anText = juce::String::fromUTF8 ("analysis failed: ") + err;
                    storeAnalysis (i, n);
                    lastErr = err;
                    if (fatal) break;
                    continue;
                }
                if (n.drum) drumAnalysis (n); else analyzeJob (n);
                storeAnalysis (i, n);
                if (n.analyzed) ++ok;
            }
            send (juce::MidiMessage::allNotesOff (settings.channel));
            if (threadShouldExit()) setStatus ("Stopped.");
            else if (ok == (int) analyzeRows.size())
                setStatus (juce::String::fromUTF8 ("Analysis done: the values are in the list on the right, next to the sound name, and are used when recording."));
            else
                setStatus (juce::String::fromUTF8 ("Analysis: succeeded ") + juce::String (ok) + " of " + juce::String ((int) analyzeRows.size())
                           + (lastErr.isNotEmpty() ? " (" + lastErr + ")" : juce::String::fromUTF8 (" - nothing is heard: check the audio input (Kronos USB), the MIDI channel and that the INPUT meter moves")));
            return;
        }

        for (int i = 0; ! threadShouldExit(); ++i)
        {
            // lista moze da raste dok se snima (dodato na kraj se snima posle)
            NeededMs n;
            std::vector<int> jobVels;
            int total = 0;
            {
                const juce::ScopedLock sl (jobsLock);
                total = (int) needed.size();
                if (i >= total) break;
                n = needed[(size_t) i];
                if (i < (int) exiVels.size()) jobVels = exiVels[(size_t) i];
            }
            if ((n.exi || n.external || n.drum) && jobVels.empty()) jobVels = exiVelocities (n);
            if (isDone (n)) continue;
            setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  (" + n.usedBy + ")");
            juce::String err; bool fatal = false;
            const int files0 = repFiles.load(); const auto bytes0 = repBytes.load();
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto addReport = [&] (const juce::String& result)
            {
                ReportLine L; L.label = n.label(); L.usedBy = n.usedBy; L.result = result;
                L.files = repFiles.load() - files0; L.mb = (repBytes.load() - bytes0) / 1048576.0;
                L.seconds = (juce::Time::getMillisecondCounterHiRes() - t0) * 0.001;
                report.push_back (L);
            };
            if (! selectJob (n, err, fatal)) { addReport ("ERROR: " + err); setStatus (err); if (fatal) break; continue; }
            if (n.drum) { drumAnalysis (n); storeAnalysis (i, n); }
            else if (autoAnalyze && ! n.analyzed)
            {
                setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  analyzing...");
                analyzeJob (n);
                storeAnalysis (i, n);
            }
            if (threadShouldExit()) break;
            prepareFolder (n);

            // procenjene vrednosti vaze samo za ovaj zvuk
            const auto saved = settings;
            if (n.analyzed)
            {
                settings.hold = n.anHold;
                settings.release = n.anRelease;
                settings.autoLoop = settings.autoLoop && n.anLoop;
            }

            if (n.drum)
            {
                // bubanj: kratak udarac, pa se snima dok zvuk sam ne utihne (do 15 s), bez loop-a
                settings.autoLoop = false;
                settings.hold = 0.3;
                settings.release = 15.0;
                recordDrum (n, i, total, jobVels);
            }
            else if (n.exi || n.external)
            {
                auto vels = jobVels;
                if (n.analyzed && n.anVelFlat) vels = { 127 };   // dinamika ne menja zvuk
                recordExi (n, i, total, vels);
            }
            else
            {
                const auto folder = msFolder (n);
                folder.createDirectory();
                int written = 0;
                // ROM multisempl: prvo nadji zone (gde Kronos prelazi na sledeci sempl), pa po jedan ton u zoni
                std::vector<Zone> zones;
                if (settings.zones)
                {
                    setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  finding zones...");
                    zones = detectZones (n, folder, i, total);
                }
                std::vector<int> notes;
                if (! zones.empty()) for (auto& z : zones) notes.push_back (z.root);
                else for (int note = settings.low; note <= settings.high; note += settings.step) notes.push_back (note);
                // ime sempla mora biti dozvoljeno na Windows-u (npr. "sfp<" iz Kronos imena ne sme u ime fajla)
                const auto fileOf = [&] (int note) { return juce::File::createLegalFileName (n.label()).replaceCharacter (' ', '_') + " " + juce::String (note).paddedLeft ('0', 3) + ".wav"; };
                for (size_t k = 0; k < notes.size() && ! threadShouldExit(); ++k)
                {
                    const int note = notes[k];
                    setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  note " + noteName (note)
                               + (zones.empty() ? juce::String() : "  (zone " + juce::String ((int) k + 1) + "/" + juce::String ((int) zones.size()) + ")"));
                    if (recordNote (note, folder.getChildFile (fileOf (note))))
                        ++written;
                }
                if (! threadShouldExit())
                {
                    if (written > 0)
                    {
                        folder.getChildFile ("kronos-ms.txt").replaceWithText (n.romKey + "\n" + n.label() + "\n" + n.usedBy + "\n");
                        if (! zones.empty())
                        {
                            // tacna mapa zona za Pikaso Workstation (LiveSampler-mapa.xml u folderu)
                            juce::XmlElement map ("LiveSamplerMap");
                            map.setAttribute ("version", 1);
                            for (size_t k = 0; k < zones.size(); ++k)
                            {
                                if (! folder.getChildFile (fileOf (zones[k].root)).existsAsFile()) continue;
                                auto* e = map.createNewChildElement ("Zone");
                                e->setAttribute ("sample", fileOf (zones[k].root));
                                e->setAttribute ("enabled", 1);
                                e->setAttribute ("lowKey", k == 0 ? 0 : zones[k].lo);
                                e->setAttribute ("highKey", k + 1 == zones.size() ? 127 : zones[k].hi);
                                e->setAttribute ("lowVel", 1);
                                e->setAttribute ("highVel", 127);
                                e->setAttribute ("root", zones[k].root);
                                e->setAttribute ("loop", 1);
                            }
                            map.writeTo (folder.getChildFile ("LiveSampler-mapa.xml"));
                        }
                    }
                    else
                        folder.deleteRecursively();   // nista se ne cuje (npr. EXs nije instaliran)
                }
            }
            settings = saved;
            addReport (threadShouldExit() ? juce::String ("stopped") : repFiles.load() > files0 ? juce::String ("OK") : juce::String::fromUTF8 ("NOTHING HEARD (silent / not installed) - not saved"));
            if (threadShouldExit()) break;
            juce::MessageManager::callAsync ([this] { neededList.repaint(); updateInfo(); });
        }
        send (juce::MidiMessage::allNotesOff (settings.channel));
        const auto rep = writeReport (threadShouldExit());
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe == nullptr) return;
            // nit je mozda jos u zavrsnom delu: sacekaj da se ugasi pre menjanja liste
            juce::Timer::callAfterDelay (300, [safe]
            {
                if (safe == nullptr || safe->isThreadRunning()) return;
                safe->tidyRomFolders();
                const auto cmds = safe->pendingCommands;
                safe->pendingCommands.clear();
                for (auto& c : cmds) safe->applyCommandLine (c);
                safe->neededList.repaint();
                safe->updateInfo();
            });
        });
        setStatus ((threadShouldExit() ? juce::String ("Stopped. ") : juce::String::fromUTF8 ("Done. In Pikaso Workstation click Refresh (SETTINGS), then create the users from the Kronos programs again. ")) + rep);
    }

    // izvestaj: tekst fajl u folderu semplova + kratak rezime
    juce::String writeReport (bool stopped)
    {
        if (report.empty()) return {};
        int ok = 0, bad = 0, files = 0; double mb = 0, secs = 0;
        juce::String txt;
        txt << "Pikaso Autosampler - " << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M") << (stopped ? "  (STOPPED)" : "") << "\n"
            << (bitsPerSample == 16 ? "16-bit" : "24-bit") << " WAV, folder: " << outFolder.getFullPathName() << "\n\n";
        for (auto& L : report)
        {
            const bool good = L.result == "OK";
            ok += good ? 1 : 0; bad += good || L.result == "stopped" ? 0 : 1;
            files += L.files; mb += L.mb; secs += L.seconds;
            txt << (good ? "  OK   " : "  !!   ") << L.label.paddedRight (' ', 34) << juce::String (L.files).paddedLeft (' ', 4) << " files "
                << juce::String (L.mb, 1).paddedLeft (' ', 8) << " MB " << fmtTime ((int) L.seconds).paddedLeft (' ', 10)
                << (good ? juce::String() : "   " + L.result) << (L.usedBy.isNotEmpty() ? "   [" + L.usedBy + "]" : juce::String()) << "\n";
        }
        txt << "\nTotal: " << ok << " OK, " << bad << " with problems, " << files << " files, " << fmtBytes ((juce::int64) (mb * 1048576.0)) << ", " << fmtTime ((int) secs) << "\n";
        const auto f = outFolder.getChildFile ("Autosampler report " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M") + ".txt");
        f.replaceWithText (txt);
        return juce::String (ok) + " OK, " + juce::String (bad) + " problems, " + juce::String (files) + " files, " + fmtBytes ((juce::int64) (mb * 1048576.0))
             + ". Report: " + f.getFileName();
    }

    void storeAnalysis (int i, const NeededMs& n)
    {
        const auto key = n.romKey;
        const bool ok = n.analyzed, loop = n.anLoop, flat = n.anVelFlat;
        const double hold = n.anHold, rel = n.anRelease;
        const auto text = n.anText;
        juce::MessageManager::callAsync ([this, i, key, ok, loop, flat, hold, rel, text]
        {
            const juce::ScopedLock sl (jobsLock);   // nit za snimanje istovremeno kopira needed[i]
            if (i < (int) needed.size() && needed[(size_t) i].romKey == key)
            {
                auto& m = needed[(size_t) i];
                m.analyzed = ok; m.anLoop = loop; m.anVelFlat = flat; m.anHold = hold; m.anRelease = rel; m.anText = text;
            }
            neededList.repaint();
            updateInfo();
        });
    }

    // snimi jedan ton bez upisa: ostaje u recorder.buffer, vraca duzinu i mesto pustanja tastera
    int captureNote (int note, int vel, double holdSec, double maxReleaseSec, int& offPos)
    {
        for (int k = 0; k < 60 && recorder.level.load() > 0.002f && ! threadShouldExit(); ++k) wait (50);
        recorder.writePos = 0;
        recorder.recording = true;
        wait (40);
        send (juce::MidiMessage::noteOn (settings.channel, note, (juce::uint8) vel));
        wait ((int) (holdSec * 1000.0));
        offPos = recorder.writePos.load();
        send (juce::MidiMessage::noteOff (settings.channel, note));
        const auto t0 = juce::Time::getMillisecondCounter();
        wait (150);
        while (! threadShouldExit() && juce::Time::getMillisecondCounter() - t0 < (juce::uint32) (maxReleaseSec * 1000.0))
        {
            if (recorder.level.load() < 0.0003f) break;
            wait (50);
        }
        wait (100);
        recorder.recording = false;
        const int len = recorder.writePos.load();
        juce::Logger::writeToLog ("  captured note " + juce::String (note) + " vel " + juce::String (vel) + ": " + juce::String (len) + " samples, off at " + juce::String (offPos)
                                  + ", level " + juce::String (recorder.level.load(), 4));
        return len;
    }

    struct Zone { int lo, hi, root; };

    // Zone ROM multisempla: svaki taster se kratko odsvira. Unutar iste zone Kronos svira isti sempl samo
    // ubrzan za polustepen, pa je snimak tona k+1 skoro isti kao snimak tona k ubrzan 2^(1/12) puta.
    // Gde se to ne poklapa, pocinje sledeci sempl (nova zona). Za svaku zonu se snima jedan ton (sredina zone).
    std::vector<Zone> detectZones (const NeededMs& n, const juce::File& folder, int jobIndex, int total)
    {
        const double sr = recorder.sampleRate;
        const int lo = settings.low, hi = settings.high;
        const int pre = (int) (sr * 0.004), win = (int) (sr * 0.10), extra = (int) (sr * 0.03);
        std::vector<std::vector<float>> rec ((size_t) 128);
        for (int note = lo; note <= hi && ! threadShouldExit(); ++note)
        {
            setStatus (juce::String (jobIndex + 1) + "/" + juce::String (total) + "  " + n.label() + "  finding zones: " + noteName (note));
            int offPos = 0;
            const int len = captureNote (note, 127, 0.35, 0.35, offPos);
            const auto& buf = recorder.buffer;
            float peak = 0.0f;
            for (int k = 0; k < juce::jmin (len, offPos); ++k)
                peak = juce::jmax (peak, std::abs (buf.getSample (0, k) + buf.getSample (1, k)));
            if (peak < 2.0e-4f) continue;                     // ne cuje se
            // pocetak sempla (ne pocetak glasnog dela): unutar zone se ceo sempl ubrzava od note-on-a, pa poredimo od tog trenutka
            float noise = 0.0f;
            for (int k = 0; k < juce::jmin (len, (int) (sr * 0.03)); ++k)
                noise = juce::jmax (noise, std::abs (buf.getSample (0, k) + buf.getSample (1, k)));
            const float th = juce::jmax (peak * 0.003f, noise * 4.0f, 2.0e-5f);
            int on = 0;
            while (on < len && std::abs (buf.getSample (0, on) + buf.getSample (1, on)) < th) ++on;
            const int start = juce::jmax (0, on - pre), need = pre + win + 2 * extra;
            if (start + need > len) continue;
            std::vector<float> m ((size_t) need);
            for (int k = 0; k < need; ++k)
            {
                // mono, blago izravnato (manja razlika u interpolaciji visokih tonova)
                float acc = 0.0f;
                for (int d = -1; d <= 1; ++d)
                {
                    const int idx = juce::jlimit (0, len - 1, start + k + d);
                    acc += buf.getSample (0, idx) + buf.getSample (1, idx);
                }
                m[(size_t) k] = acc / 6.0f;
            }
            rec[(size_t) note] = std::move (m);
        }
        if (threadShouldExit()) return {};

        // slicnost susednih tonova: b (ton k+1) prema a (ton k) ubrzanom za polustepen, najbolji pomak +-3 ms
        const double ratio = std::pow (2.0, 1.0 / 12.0);
        const int maxLag = (int) (sr * 0.003);
        auto similarity = [&] (const std::vector<float>& a, const std::vector<float>& b)
        {
            const int n2 = pre + win;
            std::vector<float> as ((size_t) (n2 + 2 * maxLag + 2));
            for (size_t k = 0; k < as.size(); ++k)
            {
                const double pos = pre + ((double) k - pre) * ratio;   // ubrzano oko pocetka sempla
                const int i0 = (int) pos;
                const float f = (float) (pos - i0);
                as[k] = (i0 >= 0 && i0 + 1 < (int) a.size()) ? a[(size_t) i0] * (1.0f - f) + a[(size_t) i0 + 1] * f : 0.0f;
            }
            double best = -1.0;
            for (int lag = -maxLag; lag <= maxLag; ++lag)
            {
                double sab = 0, saa = 0, sbb = 0;
                for (int k = 0; k < n2; ++k)
                {
                    const int ia = k + maxLag + lag, ib = k + maxLag;
                    if (ia < 0 || ia >= (int) as.size() || ib >= (int) b.size()) continue;
                    const double x = as[(size_t) ia], y = b[(size_t) ib];
                    sab += x * y; saa += x * x; sbb += y * y;
                }
                if (saa > 0 && sbb > 0) best = juce::jmax (best, sab / std::sqrt (saa * sbb));
            }
            return best;
        };

        juce::String log = "note -> similarity with the previous note (1.0 = same sample)\n";
        std::vector<Zone> zones;
        int zoneStart = -1, prev = -1;
        // izmereno na Kronos snimcima: isti sempl >= 0.996 (i 0.9998 kod EP-a), susedni razliciti semplovi <= 0.988
        const double threshold = 0.992;
        for (int note = lo; note <= hi; ++note)
        {
            if (rec[(size_t) note].empty()) continue;
            if (zoneStart < 0) { zoneStart = note; prev = note; continue; }
            const double sim = prev == note - 1 ? similarity (rec[(size_t) prev], rec[(size_t) note]) : 0.0;
            log << noteName (note) << " (" << note << "): " << juce::String (sim, 4) << (sim < threshold ? "   <- new zone" : "") << "\n";
            if (sim < threshold)
            {
                zones.push_back ({ zoneStart, prev, (zoneStart + prev) / 2 });
                zoneStart = note;
            }
            prev = note;
        }
        if (zoneStart >= 0) zones.push_back ({ zoneStart, prev, (zoneStart + prev) / 2 });

        // mesovito: zona od bar 3 tona je sigurna (jedan snimak). Gde su "zone" od 1-2 tona (zvuk se ne poklapa
        // pouzdano, npr. svetli metalni zvuci na visokim tonovima), tu se snima na svaki korak kao ranije.
        const int stepN = juce::jmax (1, settings.step);
        std::vector<Zone> out;
        int runLo = -1, runHi = -1;
        auto flushRun = [&]
        {
            if (runLo < 0) return;
            for (int a = runLo; a <= runHi; a += stepN)
            {
                const int b = juce::jmin (runHi, a + stepN - 1);
                out.push_back ({ a, b, (a + b) / 2 });
            }
            runLo = runHi = -1;
        };
        int sure = 0;
        for (auto& z : zones)
        {
            if (z.hi - z.lo + 1 >= 3) { flushRun(); out.push_back (z); ++sure; }
            else { if (runLo < 0) runLo = z.lo; runHi = z.hi; }
        }
        flushRun();
        for (auto& z : out)
            log << "zone " << noteName (z.lo) << "-" << noteName (z.hi) << "  record " << noteName (z.root)
                << (z.hi - z.lo + 1 >= 3 && std::any_of (zones.begin(), zones.end(), [&z] (const Zone& q) { return q.lo == z.lo && q.hi == z.hi; }) ? "   (sure)" : "   (every " + juce::String (stepN) + " notes)") << "\n";
        log << juce::String (sure) << " sure zones, " << juce::String ((int) out.size()) << " notes to record\n";
        folder.getChildFile ("kronos-zones.txt").replaceWithText (log);
        return out;
    }

    // procena na tri tona (nisko, sredina, visoko): drzi li zvuk (loop), koliko traje, koliki je release
    void analyzeJob (NeededMs& n)
    {
        const double testHold = 12.0, testRelease = 10.0;
        const double sr = recorder.sampleRate;
        const int lo = settings.low, hi = settings.high;
        std::vector<int> notes { juce::jlimit (lo, hi, lo + 12), (lo + hi) / 2, juce::jlimit (lo, hi, hi - 12) };
        notes.erase (std::unique (notes.begin(), notes.end()), notes.end());

        int sustainedCount = 0, heard = 0;
        double holdNeed = 0, relNeed = 0;
        for (int note : notes)
        {
            if (threadShouldExit()) return;
            setStatus ("Analysis: " + n.label() + "  note " + noteName (note));
            int offPos = 0;
            const int len = captureNote (note, 127, testHold, testRelease, offPos);
            const int blk = juce::jmax (64, (int) (sr * 0.01));
            const int nb = len / blk, offB = juce::jlimit (0, nb, offPos / blk);
            if (nb < 50 || offB < 50) continue;
            const auto& buf = recorder.buffer;
            std::vector<double> env ((size_t) nb);
            for (int b = 0; b < nb; ++b)
            {
                double e = 0;
                for (int k = b * blk; k < (b + 1) * blk; ++k)
                    e += 0.5 * (buf.getSample (0, k) * buf.getSample (0, k) + buf.getSample (1, k) * buf.getSample (1, k));
                env[(size_t) b] = std::sqrt (e / blk) + 1.0e-9;
            }
            double peak = 0; int peakB = 0;
            for (int b = 0; b < offB; ++b) if (env[(size_t) b] > peak) { peak = env[(size_t) b]; peakB = b; }
            if (peak < 1.0e-4) continue;    // ne cuje se (nota van opsega)
            ++heard;
            int onset = 0;
            while (onset < peakB && env[(size_t) onset] < peak * 0.01) ++onset;
            auto avg = [&env] (int a, int b) { double s = 0; int c = 0; for (int k = juce::jmax (0, a); k < b; ++k, ++c) s += env[(size_t) k]; return c > 0 ? s / c : 1.0e-9; };
            const double endHold = avg (offB - 30, offB);
            const double earlier = avg (offB - 150, offB - 100);
            const double dropDb = 20.0 * std::log10 (endHold / peak);
            const double slope = 20.0 * std::log10 (endHold / earlier);     // dB za ~1.25 s
            // drzi: na kraju vise ne opada (i kad je posle attack-a pao na nizi nivo, npr. zvonca / pad sa loop-om)
            const bool sustained = dropDb > -40.0 && slope > -3.0;
            double hold;
            if (sustained)
            {
                ++sustainedCount;
                // kad je zvuk prvi put stigao do nivoa na kome drzi (posle attack-a i decay-a).
                // Envelopa se izravna na 300 ms, da vibrato / tremolo / LFO ne produze procenu.
                // Posle toga treba jos samo mesta za loop (~1.5 s), pa drzanje ide 2-5 s, ne 10.
                std::vector<double> sm ((size_t) offB, 0.0);
                for (int b = 0; b < offB; ++b)
                {
                    double acc = 0; int cnt = 0;
                    for (int k = juce::jmax (0, b - 15); k < juce::jmin (offB, b + 15); ++k, ++cnt) acc += env[(size_t) k];
                    sm[(size_t) b] = acc / juce::jmax (1, cnt);
                }
                // nivo na kome drzi: srednja vrednost 0.5-3 s posle vrha (ne kraj od 12 s, jer zvuk koji polako
                // opada, gudaci / vibrafon, tada nikad ne "stigne" do kraja pa bi drzanje islo do maksimuma)
                std::vector<double> mid;
                for (int b = peakB + 50; b < juce::jmin (offB, peakB + 300); ++b) mid.push_back (sm[(size_t) b]);
                double ref = endHold;
                if (! mid.empty()) { std::nth_element (mid.begin(), mid.begin() + (long) mid.size() / 2, mid.end()); ref = mid[mid.size() / 2]; }
                int settle = peakB;
                while (settle < offB - 1 && std::abs (20.0 * std::log10 (sm[(size_t) settle] / ref)) > 3.0) ++settle;
                // posle toga samo mesto za loop (~1 s): cist zvuk 1.2 s, najvise 3 s (manje mesta na disku)
                hold = juce::jlimit (1.2, 3.0, (settle - onset) * 0.01 + 1.0);
            }
            // sum ulaza (pre tona): kratak zvuk (klik klapne, key off sum) se zavrsi kad padne do suma, ne do -50 dB
            double floorLvl = 1.0e-5;
            if (onset > 3) floorLvl = juce::jmax (floorLvl, avg (0, onset) * 2.0);
            bool endedEarly = false;
            if (! sustained)
            {
                int end = peakB;
                while (end < offB && env[(size_t) end] > juce::jmax (peak * 0.003, floorLvl * 1.5)) ++end;   // -50 dB ili sum
                endedEarly = end < offB;
                hold = endedEarly ? (end - onset) * 0.01 + 0.3 : testHold;
            }
            // release: od pustanja tastera do -60 dB (ili do suma); zvuk koji je vec utihnuo nema release
            const double startRel = env[(size_t) juce::jmax (0, offB - 1)];
            int r = offB;
            while (r < nb && env[(size_t) r] > juce::jmax (startRel * 0.001, floorLvl * 1.5)) ++r;
            const double rel = ! endedEarly && startRel > floorLvl * 2.0 ? (r - offB) * 0.01 : 0.0;
            holdNeed = juce::jmax (holdNeed, hold);
            relNeed = juce::jmax (relNeed, rel);
        }
        if (heard == 0)
        {
            n.analyzed = false;
            n.anText = juce::String::fromUTF8 ("analysis: nothing is heard (audio input / MIDI channel?)");
            return;
        }
        // dinamika: isti ton na jacini 127 i 40. Ako je glasnoca i boja ista (dinamika iskljucena u zvuku),
        // dovoljan je jedan velocity sloj.
        n.anVelFlat = false;
        if ((n.exi || n.external) && ! threadShouldExit())
        {
            const int note = notes[notes.size() / 2];
            double lv[2] = {}, br[2] = {};
            bool okV = true;
            const int velsTest[2] = { 127, 40 };
            for (int k = 0; k < 2 && okV && ! threadShouldExit(); ++k)
            {
                setStatus ("Analysis: " + n.label() + juce::String::fromUTF8 ("  velocity ") + juce::String (velsTest[k]));
                int offPos = 0;
                const int len = captureNote (note, velsTest[k], 1.5, 3.0, offPos);
                const auto& buf = recorder.buffer;
                double peakAbs = 0;
                int on = 0;
                for (int s = 0; s < offPos; ++s) peakAbs = juce::jmax (peakAbs, (double) std::abs (buf.getSample (0, s)));
                while (on < offPos && std::abs (buf.getSample (0, on)) < peakAbs * 0.05) ++on;
                const int end = juce::jmin (offPos, len, on + (int) (sr * 1.0));
                double e = 0, d = 0;
                for (int s = on + 1; s < end; ++s)
                    for (int c = 0; c < 2; ++c)
                    {
                        const double x = buf.getSample (c, s), dx = x - buf.getSample (c, s - 1);
                        e += x * x; d += dx * dx;
                    }
                if (end - on < (int) (sr * 0.2) || e < 1.0e-6) { okV = false; break; }
                lv[k] = 10.0 * std::log10 (e / (end - on));
                br[k] = d / e;          // vise visokih tonova = veci odnos
            }
            if (okV && ! threadShouldExit())
            {
                const double levelDiff = std::abs (lv[0] - lv[1]);
                const double brightDiff = std::abs (br[0] - br[1]) / juce::jmax (1.0e-9, br[0]);
                n.anVelFlat = levelDiff < 1.5 && brightDiff < 0.08;
            }
        }

        n.analyzed = true;
        n.anLoop = sustainedCount * 2 > heard;
        n.anHold = std::round (holdNeed * 10.0) / 10.0;
        n.anRelease = juce::jlimit (0.3, 15.0, std::round ((relNeed + 0.3) * 10.0) / 10.0);
        n.anText = juce::String::fromUTF8 ("hold ") + juce::String (n.anHold, 1) + " s, release " + juce::String (n.anRelease, 1) + " s, "
                 + (n.anLoop ? "loop" : juce::String::fromUTF8 ("no loop (sound decays)"))
                 + (n.anVelFlat ? juce::String::fromUTF8 (", velocity does not change the sound: 1 layer") : juce::String());
    }

    // drum kit se ne procenjuje: svaki taster je jedan udarac (one-shot) koji se snima do kraja, bez loop-a
    void drumAnalysis (NeededMs& n)
    {
        n.analyzed = true;
        n.anLoop = false;
        n.anVelFlat = false;
        n.anHold = 0.3;
        n.anRelease = 15.0;
        n.anText = "drum kit: every key one-shot until it fades out, no loop";
    }

    // drum kit: svaki taster posebno (svoja zona, bez pomeranja visine), u vise jacina; tasteri bez zvuka se preskacu
    void recordDrum (const NeededMs& n, int i, int total, const std::vector<int>& velsIn)
    {
        auto vels = velsIn;
        if (vels.empty()) vels = { 127 };
        std::sort (vels.begin(), vels.end());
        const auto folder = msFolder (n);
        folder.createDirectory();
        juce::XmlElement map ("LiveSamplerMap");
        map.setAttribute ("version", 1);
        int written = 0;
        // prvo kratko proveri koji tasteri uopste imaju zvuk (drum kit retko koristi celu klavijaturu)
        // Taster ima zvuk samo ako posle note-on dodje nov udarac: rep prethodnog bubnja (cinela) i sum ulaza se ne racunaju.
        // Snimak pocinje 40 ms pre note-on, pa se to uzima kao nivo pre udarca.
        std::vector<bool> sounding ((size_t) 128, false);
        std::vector<float> pre ((size_t) 128, 0.0f), post ((size_t) 128, 0.0f);
        const int preLen = (int) (recorder.sampleRate * 0.035);
        for (int note = settings.low; note <= settings.high && ! threadShouldExit(); ++note)
        {
            setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  checking key " + noteName (note));
            // sacekaj da rep prethodnog udarca utihne (do 2 s)
            for (int k = 0; k < 40 && recorder.level.load() > 0.0005f && ! threadShouldExit(); ++k) wait (50);
            int offPos = 0;
            const int len = captureNote (note, 127, 0.25, 0.3, offPos);
            float a = 0.0f, b = 0.0f;
            for (int k = 0; k < len; ++k)
            {
                const float s = juce::jmax (std::abs (recorder.buffer.getSample (0, k)), std::abs (recorder.buffer.getSample (1, k)));
                (k < preLen ? a : b) = juce::jmax (k < preLen ? a : b, s);
            }
            pre[(size_t) note] = a;
            post[(size_t) note] = b;
        }
        float loudest = 0.0f;
        for (auto v : post) loudest = juce::jmax (loudest, v);
        juce::StringArray empty;
        for (int note = settings.low; note <= settings.high; ++note)
        {
            const float a = pre[(size_t) note], b = post[(size_t) note];
            // nov udarac: bar 10 dB iznad nivoa pre note-on, iznad suma (-50 dB) i ne vise od 46 dB ispod najjaceg bubnja
            sounding[(size_t) note] = b > a * 3.2f && b > 0.003f && b > loudest * 0.005f;
            if (! sounding[(size_t) note]) empty.add (noteName (note));
        }
        juce::Logger::writeToLog ("drum " + n.label() + ": empty keys " + empty.joinIntoString (" "));
        // WAV-ovi praznih tastera iz ranijeg snimanja se brisu (u Recycle Bin)
        for (int note = settings.low; note <= settings.high && ! threadShouldExit(); ++note)
            if (! sounding[(size_t) note])
                for (auto& f : folder.findChildFiles (juce::File::findFiles, false, juce::String (note).paddedLeft ('0', 3) + " v*.wav"))
                    if (! f.moveToTrash()) f.deleteFile();
        // bubanj: ako procena nije dala drzanje, dovoljno je 1.5 s (zvuk se snima i posle pustanja tastera)
        for (int note = settings.low; note <= settings.high && ! threadShouldExit(); ++note)
        {
            if (! sounding[(size_t) note]) continue;
            bool sound = true;
            // najjaca jacina prva: ako taster cuti, ostale jacine se ne snimaju
            for (int v = (int) vels.size() - 1; v >= 0 && sound && ! threadShouldExit(); --v)
            {
                const int vel = vels[(size_t) v];
                setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  key " + noteName (note) + "  velocity " + juce::String (vel));
                const auto name = juce::String (note).paddedLeft ('0', 3) + " v" + juce::String (vel).paddedLeft ('0', 3) + ".wav";
                const auto f = folder.getChildFile (name);
                const bool ok = f.existsAsFile() || recordNote (note, f, vel);
                if (! ok) { if (v == (int) vels.size() - 1) sound = false; continue; }
                ++written;
                auto* e = map.createNewChildElement ("Zone");
                e->setAttribute ("sample", name);
                e->setAttribute ("enabled", 1);
                e->setAttribute ("lowKey", note);
                e->setAttribute ("highKey", note);
                e->setAttribute ("lowVel", v == 0 ? 1 : vels[(size_t) v - 1] + 1);
                e->setAttribute ("highVel", v + 1 == (int) vels.size() ? 127 : vel);
                e->setAttribute ("root", note);
                e->setAttribute ("loop", 0);
            }
        }
        if (threadShouldExit()) return;
        if (written == 0) { folder.deleteRecursively(); return; }
        map.writeTo (folder.getChildFile ("LiveSampler-mapa.xml"));
        folder.getChildFile ("kronos-ms.txt").replaceWithText (n.romKey + "\n" + n.label() + "\n" + n.usedBy + "\n"
                                                               + "layers=" + juce::String ((int) vels.size()) + "\n" + "engine=drum\n");
    }

    // ceo EXi program: svaki korak note x svaka jacina; release se meri i upisuje u kronos-ms.txt
    void recordExi (const NeededMs& n, int i, int total, const std::vector<int>& vels)
    {
        const auto folder = msFolder (n);
        folder.createDirectory();
        int written = 0;
        std::vector<double> releases;
        for (int note = settings.low; note <= settings.high && ! threadShouldExit(); note += settings.step)
            for (size_t v = 0; v < vels.size() && ! threadShouldExit(); ++v)
            {
                const int vel = vels[v];
                setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  note " + noteName (note)
                           + juce::String::fromUTF8 ("  velocity ") + juce::String (vel));
                // ime: nota (broj) + v<jacina>; Pikaso Workstation od toga pravi velocity zone
                const auto f = folder.getChildFile (juce::String (note).paddedLeft ('0', 3) + " v" + juce::String (vel).paddedLeft ('0', 3) + ".wav");
                double rel = -1.0;
                if (f.existsAsFile()) { ++written; continue; }
                if (recordNote (note, f, vel, &rel))
                {
                    ++written;
                    if (rel >= 0.0 && v + 1 == vels.size()) releases.push_back (rel);
                }
            }
        if (threadShouldExit()) return;
        if (written == 0) { folder.deleteRecursively(); return; }
        double release = 0.3;
        if (! releases.empty())
        {
            std::sort (releases.begin(), releases.end());
            release = releases[releases.size() / 2];
        }
        folder.getChildFile ("kronos-ms.txt").replaceWithText (n.romKey + "\n" + n.label() + "\n" + n.usedBy + "\n"
                                                               + "release=" + juce::String (release, 3) + "\n"
                                                               + "layers=" + juce::String ((int) vels.size()) + "\n"
                                                               + "engine=" + n.engine + "\n");
    }

    bool recordNote (int note, const juce::File& file, int velocityOverride = -1, double* releaseSec = nullptr)
    {
        const double sr = recorder.sampleRate;
        // sacekaj tisinu pre note
        for (int k = 0; k < 40 && recorder.level.load() > 0.002f && ! threadShouldExit(); ++k) wait (50);

        recorder.writePos = 0;
        recorder.recording = true;
        wait (40);
        const int vel = velocityOverride > 0 ? velocityOverride : settings.velocity;
        send (juce::MidiMessage::noteOn (settings.channel, note, (juce::uint8) vel));
        wait ((int) (settings.hold * 1000.0));
        const int offPos = recorder.writePos.load();
        send (juce::MidiMessage::noteOff (settings.channel, note));
        // release: dok ne utihne ili do maksimuma
        const auto t0 = juce::Time::getMillisecondCounter();
        wait (150);
        while (! threadShouldExit() && juce::Time::getMillisecondCounter() - t0 < (juce::uint32) (settings.release * 1000.0))
        {
            if (recorder.level.load() < 0.0005f) break;
            wait (50);
        }
        recorder.recording = false;
        const int len = recorder.writePos.load();
        if (threadShouldExit() || len < (int) (sr * 0.1)) return false;

        auto& buf = recorder.buffer;
        float peak = 0.0f;
        for (int c = 0; c < 2; ++c) peak = juce::jmax (peak, buf.getMagnitude (c, 0, len));
        if (peak < 0.0005f) return false;   // tisina (nota van opsega multisempla)

        // isecanje tisine na pocetku (ostavi 2 ms) i na kraju, kratki fade na kraju
        int start = AutoLoop::findOnset (buf, len, sr);     // tisina / sum pre tona se odseca (i kod tihih jacina)
        int end = len;
        const float endThr = peak * 0.0003f;
        while (end > start + 64 && std::abs (buf.getSample (0, end - 1)) < endThr && std::abs (buf.getSample (1, end - 1)) < endThr) --end;
        const int outLen = end - start;
        const int holdLen = juce::jlimit (0, outLen, offPos - start);   // deo pre pustanja tastera
        if (releaseSec != nullptr && holdLen > 0)
        {
            // vreme za koje zvuk posle pustanja tastera padne 30 dB
            auto rms = [&] (int from, int w)
            {
                double e = 0; int cnt = 0;
                for (int k = juce::jmax (0, from); k < juce::jmin (len, from + w); ++k, ++cnt)
                    e += 0.5 * (buf.getSample (0, k) * buf.getSample (0, k) + buf.getSample (1, k) * buf.getSample (1, k));
                return cnt > 0 ? std::sqrt (e / cnt) : 0.0;
            };
            const int w = (int) (sr * 0.01);
            const double l0 = rms (offPos - 2 * w, 2 * w);
            int k = offPos;
            if (l0 > 1.0e-5)
                while (k < end && rms (k, w) > l0 * 0.0316) k += w;
            *releaseSec = (double) (k - offPos) / sr;
        }
        const int fade = juce::jmin (outLen / 4, (int) (sr * 0.01));
        juce::AudioBuffer<float> outBuf (2, outLen);
        for (int c = 0; c < 2; ++c)
        {
            outBuf.copyFrom (c, 0, buf, c, start, outLen);
            outBuf.applyGainRamp (c, outLen - fade, fade, 1.0f, 0.0f);
        }

        file.deleteFile();
        auto fos = std::make_unique<juce::FileOutputStream> (file);
        if (fos->failedToOpen()) return false;   // npr. nedozvoljeno ime: ne broji se kao snimljen
        std::unique_ptr<juce::OutputStream> os = std::move (fos);
        juce::WavAudioFormat wav;
        // auto loop: nadji loop u delu koji drzi, upisi crossfade u sempl i loop tacke u WAV (smpl)
        std::unordered_map<juce::String, juce::String> meta;
        meta["MidiUnityNote"] = juce::String (note);
        if (settings.autoLoop)
        {
            AutoLoop::Settings ls;
            ls.crossfadeSec = settings.xfadeMs * 0.001;
            ls.minLoopSec = settings.minLoopMs * 0.001;
            ls.maxLoopSec = juce::jmax (ls.minLoopSec + 0.1, settings.hold * 0.6);
            const auto lr = AutoLoop::find (outBuf, holdLen > 0 ? holdLen : outLen, sr, ls);
            if (lr.found)
            {
                AutoLoop::bakeCrossfade (outBuf, lr.start, lr.end, lr.crossfade);
                meta["NumSampleLoops"] = "1";
                meta["Loop0Type"] = "0";
                meta["Loop0Start"] = juce::String (lr.start);
                meta["Loop0End"] = juce::String (lr.end - 1);
            }
        }
        auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (bitsPerSample)
                                                   .withMetadataValues (meta));
        if (writer == nullptr) return false;
        writer->writeFromAudioSampleBuffer (outBuf, 0, outLen);
        repFiles.fetch_add (1);
        repBytes.fetch_add ((juce::int64) outLen * 2 * (bitsPerSample / 8));
        return true;
    }

    void timerCallback() override
    {
        {
            const juce::ScopedLock sl (statusLock);
            progress.setText (statusText, juce::dontSendNotification);
        }
        if (! isThreadRunning() && startButton.getButtonText() == "STOP")
            startButton.setButtonText ("START");
        repaint (meter.expanded (2, 16));
    }

    //==========================================================================
    std::unique_ptr<juce::PropertiesFile> props;
    juce::AudioDeviceManager deviceManager;
    Recorder recorder;
    std::unique_ptr<juce::MidiOutput> midi;
    juce::Array<juce::MidiDeviceInfo> midiDevices;

    std::unique_ptr<Kronos::Pcg> pcg;
    juce::MemoryBlock pcgData;
    juce::File pcgFile, outFolder;
    std::vector<RawBank> banks;
    std::vector<int> sourceRows;
    std::vector<NeededMs> needed;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> sec1, sec2, sec3, meter;
    juce::Label title1, title2, title3, fileLabel, info, templateLabel, bankLabel, startLabel, midiLabel, folderLabel, progress, help;
    juce::Label chLabel, cc0Label, cc32Label, lowLabel, highLabel, stepLabel, velLabel, holdLabel, relLabel;
    juce::Slider channel, cc0, cc32, lowNote, highNote, step, velocity, hold, release, startNum;
    juce::TextButton externalButton, analyzeButton, romButton;
    juce::ToggleButton autoAnalyzeToggle;
    bool analyzeOnly = false, autoAnalyze = true;
    std::vector<int> analyzeRows;
    juce::TextButton loadButton, addButton, clearButton, removeButton, makePcgButton, audioButton, folderButton, testButton, startButton;
    juce::ToggleButton showCombis;
    juce::TextEditor search;
    juce::ListBox sourceList, neededList;
    juce::ComboBox templateBox, bankBox, midiOut, midiInBox;
    juce::ToggleButton sysexMode, autoLoopToggle, exiIfxToggle, zoneToggle, bits16Toggle;
    // izvestaj snimanja
    struct ReportLine { juce::String label, usedBy, result; int files = 0; double mb = 0, seconds = 0; };
    std::vector<ReportLine> report;
    std::atomic<int> repFiles { 0 };
    std::atomic<juce::int64> repBytes { 0 };
    int bitsPerSample = 24;
    juce::Label exiVelLabel;
    juce::ComboBox exiVelBox;
    std::vector<std::vector<int>> exiVels;
    // lista se moze dopunjavati dok traje snimanje: nit uzima posao po posao pod ovom bravom
    juce::CriticalSection jobsLock;
    void pushJob (NeededMs n)
    {
        auto vels = n.exi || n.external || n.drum ? exiVelocities (n) : std::vector<int> {};
        const juce::ScopedLock sl (jobsLock);
        if (exiVels.size() < needed.size()) exiVels.resize (needed.size());
        needed.push_back (std::move (n));
        exiVels.push_back (std::move (vels));
    }
    void busyInfo()
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Autosampler",
            juce::String::fromUTF8 ("Sampling is running: you can add sounds to the list now (they are sampled at the end), "
                                    "but removing or clearing works only after STOP or when it is done."));
    }
    juce::Slider xfadeMs, minLoopMs;
    juce::Label xfadeLabel, minLoopLabel;
    juce::TextButton readTemplateButton;
    juce::Label templateInfo, midiInLabel;
    std::unique_ptr<juce::MidiInput> midiIn;
    juce::Array<juce::MidiDeviceInfo> midiInDevices;
    juce::CriticalSection sysexLock;
    juce::WaitableEvent sysexEvent;
    std::vector<juce::uint8> lastSysex, sxHeader, sxTemplate;
    int sxStart = 7, sxShift = 0;
    bool useSysex = true;

    struct InCallback : public juce::MidiInputCallback
    {
        MainComponent* owner = nullptr;
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override { owner->handleIncomingSysex (m); }
    } inCallback;
    juce::CriticalSection statusLock;
    juce::String statusText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class AutosamplerApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Pikaso Autosampler"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    static void crashHandler (void*)
    {
        const auto trace = juce::SystemStats::getStackBacktrace();
        juce::Logger::writeToLog ("CRASH\n" + trace);
        auto f = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("Pikaso Autosampler GRESKA.txt");
        f.replaceWithText ("Pikaso Autosampler crashed.\n\n" + trace);
    }

    void initialise (const juce::String& commandLine) override
    {
        juce::SystemStats::setApplicationCrashHandler (crashHandler);
        installCrashLog (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("LiveSampler").getChildFile ("autosampler-crash.txt").getFullPathName().toWideCharPointer());
        logger.reset (juce::FileLogger::createDefaultAppLogger ("LiveSampler", "autosampler-log.txt", "Pikaso Autosampler"));
        juce::Logger::setCurrentLogger (logger.get());
        juce::Logger::writeToLog ("start: " + commandLine);
        juce::LookAndFeel::getDefaultLookAndFeel().setColour (juce::ResizableWindow::backgroundColourId, bg);
        window = std::make_unique<Window>();
        juce::Logger::writeToLog ("window created");
        if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
            mc->applyCommandLine (commandLine);
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (window != nullptr)
            if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
            {
                window->toFront (true);
                mc->applyCommandLine (commandLine);
            }
    }
    bool moreThanOneInstanceAllowed() override { return false; }
    std::unique_ptr<juce::FileLogger> logger;
    void shutdown() override { window.reset(); juce::Logger::setCurrentLogger (nullptr); }

    struct Window : public juce::DocumentWindow
    {
        Window() : juce::DocumentWindow ("Pikaso Autosampler", bg, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (AutosamplerApp)

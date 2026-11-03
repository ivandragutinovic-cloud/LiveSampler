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
        int msb = -1, lsb = -1, pc = -1, layers = 1;
        juce::uint32 progBank = 0; int progNum = -1;     // EXi: program u PCG-u (bira se na Kronosu pre slanja)
        // procena (dugme Proceni): koliko drzati tone, release, da li treba loop
        bool analyzed = false, anLoop = true;
        double anHold = 0, anRelease = 0;
        juce::String anText;
        juce::String label() const
        {
            if (external) return "EXT " + extName;
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
class Recorder : public juce::AudioIODeviceCallback
{
public:
    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override
    {
        sampleRate = dev->getCurrentSampleRate();
        buffer.setSize (2, (int) (sampleRate * 90.0));
    }
    void audioDeviceStopped() override {}
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
        if (err.isNotEmpty()) juce::Logger::writeToLog ("audio greska: " + err);
        deviceManager.addAudioCallback (&recorder);

        auto styleTitle = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            l.setColour (juce::Label::textColourId, accent);
            addAndMakeVisible (l);
        };
        styleTitle (title1, juce::String::fromUTF8 ("1. ŠTA TREBA PRESEMPLOVATI"));
        styleTitle (title2, juce::String::fromUTF8 ("2. ŠABLON SA KRONOSA (SysEx, ništa se ne snima na Kronos; treba samo za ROM multisemplove)"));
        styleTitle (title3, "3. SNIMANJE");

        loadButton.setButtonText (juce::String::fromUTF8 ("Učitaj PCG..."));
        loadButton.onClick = [this] { choosePcg(); };
        addAndMakeVisible (loadButton);
        fileLabel.setColour (juce::Label::textColourId, lcd);
        addAndMakeVisible (fileLabel);

        search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Traži program / kombi..."), dim);
        search.onTextChange = [this] { rebuildSource(); };
        addAndMakeVisible (search);
        showCombis.setButtonText ("Kombiji");
        showCombis.onClick = [this] { rebuildSource(); };
        addAndMakeVisible (showCombis);

        sourceModel.owner = this;
        neededModel.owner = this;
        sourceList.setModel (&sourceModel);
        sourceList.setMultipleSelectionEnabled (true);
        sourceList.setColour (juce::ListBox::backgroundColourId, bg);
        addAndMakeVisible (sourceList);
        sourceModel.owner = this;

        externalButton.setButtonText (juce::String::fromUTF8 ("+ Zvuk sa druge klavijature..."));
        externalButton.setTooltip (juce::String::fromUTF8 ("Semplovanje bilo kog MIDI instrumenta (Yamaha, Roland, modul...): MIDI izlaz ka njemu i audio ulaz sa njega biraš u koraku 3."));
        externalButton.onClick = [this] { addExternal(); };
        addAndMakeVisible (externalButton);

        addButton.setButtonText (juce::String::fromUTF8 ("Dodaj izabrane →"));
        addButton.onClick = [this] { addSelected(); };
        addAndMakeVisible (addButton);
        clearButton.setButtonText (juce::String::fromUTF8 ("Očisti listu"));
        clearButton.onClick = [this] { needed.clear(); neededList.updateContent(); neededList.repaint(); updateInfo(); };
        addAndMakeVisible (clearButton);
        removeButton.setButtonText ("Ukloni izabrane");
        removeButton.onClick = [this]
        {
            auto sel = neededList.getSelectedRows();
            for (int i = sel.size(); --i >= 0;)
                if (sel[i] < (int) needed.size()) needed.erase (needed.begin() + sel[i]);
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
        addAndMakeVisible (info);

        // 2. PCG
        templateLabel.setText (juce::String::fromUTF8 ("Šablon (inicijalizovan program):"), juce::dontSendNotification);
        bankLabel.setText ("Banka za nove programe:", juce::dontSendNotification);
        startLabel.setText ("Od broja:", juce::dontSendNotification);
        for (auto* l : { &templateLabel, &bankLabel, &startLabel }) { l->setColour (juce::Label::textColourId, dim); addAndMakeVisible (l); }
        addAndMakeVisible (templateBox);
        addAndMakeVisible (bankBox);
        startNum.setRange (0, 127, 1);
        startNum.setValue (0);
        startNum.setSliderStyle (juce::Slider::IncDecButtons);
        startNum.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 24);
        addAndMakeVisible (startNum);
        makePcgButton.setButtonText ("Napravi PCG");
        makePcgButton.onClick = [this] { makePcg(); };
        // snimanje preko PCG-a (USB flash) je izbaceno: sve ide direktno preko SysEx-a
        for (juce::Component* c : { (juce::Component*) &templateLabel, (juce::Component*) &templateBox, (juce::Component*) &bankLabel,
                                    (juce::Component*) &bankBox, (juce::Component*) &startLabel, (juce::Component*) &startNum,
                                    (juce::Component*) &makePcgButton })
            c->setVisible (false);

        // 3. snimanje
        audioButton.setButtonText (juce::String::fromUTF8 ("Audio ulaz (Kronos USB)..."));
        audioButton.onClick = [this] { showAudioSettings(); };
        addAndMakeVisible (audioButton);
        midiLabel.setText ("MIDI izlaz ka Kronosu:", juce::dontSendNotification);
        addAndMakeVisible (midiOut);
        refreshMidi();
        midiOut.onChange = [this] { openMidi(); };

        midiInLabel.setText ("MIDI ulaz sa Kronosa:", juce::dontSendNotification);
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

        sysexMode.setButtonText (juce::String::fromUTF8 ("Direktno preko SysEx-a (menja trenutni program, ništa se ne snima na Kronos)"));
        sysexMode.setToggleState (true, juce::dontSendNotification);
        sysexMode.setVisible (false);
        readTemplateButton.setButtonText (juce::String::fromUTF8 ("Pročitaj šablon sa Kronosa"));
        readTemplateButton.setTooltip (juce::String::fromUTF8 ("Na Kronosu izaberi Init Program (npr. USER-FF 006), pa klikni ovde."));
        readTemplateButton.onClick = [this] { readTemplateFromKronos(); };
        addAndMakeVisible (readTemplateButton);
        templateInfo.setColour (juce::Label::textColourId, lcd);
        templateInfo.setText (juce::String::fromUTF8 ("Šablon još nije pročitan"), juce::dontSendNotification);
        addAndMakeVisible (templateInfo);

        struct F { juce::Label* l; juce::Slider* s; const char* name; double lo, hi, def; };
        const F fields[] = { { &chLabel, &channel, "MIDI kanal", 1, 16, 1 }, { &cc0Label, &cc0, "Bank CC0", 0, 127, 0 },
                             { &cc32Label, &cc32, "Bank CC32", 0, 127, 0 }, { &lowLabel, &lowNote, "Od note", 0, 127, 24 },
                             { &highLabel, &highNote, "Do note", 0, 127, 108 }, { &stepLabel, &step, "Korak (polust.)", 1, 12, 3 },
                             { &velLabel, &velocity, "Jačina", 1, 127, 127 }, { &holdLabel, &hold, "Drži (s)", 0.5, 30, 6 },
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

        exiIfxToggle.setButtonText (juce::String::fromUTF8 ("EXi: snimaj sa insert efektima (IFX)"));
        exiIfxToggle.setTooltip (juce::String::fromUTF8 ("Isključeno: suv zvuk sinteze (efekte dodaješ u Pikaso Workstation). Uključeno: zvuk sa IFX efektima programa (chorus, phaser, delay...). Master efekti (reverb) su uvek isključeni."));
        addAndMakeVisible (exiIfxToggle);
        exiVelLabel.setText (juce::String::fromUTF8 ("EXi jačine:"), juce::dontSendNotification);
        exiVelLabel.setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (exiVelLabel);
        exiVelBox.addItem (juce::String::fromUTF8 ("Auto (klavir 6, EP 5, STR-1 4, orgulje 1, sint 2)"), 1);
        for (int n : { 1, 2, 3, 4, 6, 8 }) exiVelBox.addItem (juce::String (n) + (n == 1 ? juce::String::fromUTF8 (" jačina (127)") : juce::String::fromUTF8 (" jačina")), 100 + n);
        exiVelBox.setSelectedId (1, juce::dontSendNotification);
        exiVelBox.onChange = [this] { updateInfo(); };
        addAndMakeVisible (exiVelBox);

        autoLoopToggle.setButtonText (juce::String::fromUTF8 ("Auto loop (za zvuke koji drže)"));
        autoLoopToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (autoLoopToggle);
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

        folderButton.setButtonText (juce::String::fromUTF8 ("Folder za semplove..."));
        folderButton.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (folderButton);
        folderLabel.setColour (juce::Label::textColourId, lcd);
        addAndMakeVisible (folderLabel);

        analyzeButton.setButtonText ("Proceni (izabrani)");
        analyzeButton.setTooltip (juce::String::fromUTF8 ("Odsvira 3 tona (nisko, sredina, visoko) i proceni: da li zvuk drži (loop), koliko dugo da drži taster i koliki je release. Vrednosti se koriste pri snimanju tog zvuka."));
        analyzeButton.onClick = [this] { startAnalysis(); };
        addAndMakeVisible (analyzeButton);
        autoAnalyzeToggle.setButtonText (juce::String::fromUTF8 ("Proceni sam pre snimanja"));
        autoAnalyzeToggle.setTooltip (juce::String::fromUTF8 ("Pre snimanja svakog zvuka (koji nije procenjen) prvo odsvira 3 tona i sam podesi Drži, Release i loop."));
        autoAnalyzeToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (autoAnalyzeToggle);

        testButton.setButtonText ("Test (prvi na listi)");
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
                lastKind = 0;
                ensureKronosKind (needed[0]);
                juce::String err;
                if (! sendExiSysex (needed[0], err)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "EXi", err); return; }
                send (juce::MidiMessage::noteOn ((int) channel.getValue(), 60, (juce::uint8) 100)); juce::Thread::sleep (1200); send (juce::MidiMessage::noteOff ((int) channel.getValue(), 60));
            }
            else if (sysexMode.getToggleState()) { lastKind = 0; ensureKronosKind (needed[0]); sendProgramSysex (needed[0]); send (juce::MidiMessage::noteOn ((int) channel.getValue(), 60, (juce::uint8) 127)); juce::Thread::sleep (800); send (juce::MidiMessage::noteOff ((int) channel.getValue(), 60)); }
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
            "Redosled: 1) učitaj PCG i dodaj programe/kombije (ili zvuk sa druge klavijature)  2) za ROM multisemplove: na Kronosu izaberi "
            "Init Program i klikni Pročitaj šablon  3) izaberi Kronos USB audio ulaz, MIDI izlaz i ulaz, pa START. "
            "Snimanje se može prekinuti i nastaviti: postojeći fajlovi se preskaču.\n"
            "EXi programi (AL-1, CX-3, SGX-2...) idu samo direktno preko SysEx-a (treba i MIDI ulaz sa Kronosa); šablon za njih ne treba. "
            "Na Kronosu isključi KARMA i Drum Track dugme. Za klavir i EP stavi Drži na 8-10 s. "
            "Desni klik na listu desno: Presempluj ponovo (briše stare semplove)."),
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
    void applyCommandLine (const juce::String& commandLine)
    {
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
                if (auto* p = pcg->findProgram (bank, num)) addProgram (*p);
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
        const auto f = props->getValue ("folder");
        if (f.isNotEmpty()) { outFolder = juce::File (f); folderLabel.setText (outFolder.getFullPathName(), juce::dontSendNotification); }
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
        g.drawText ("ULAZ", m.toNearestInt().translated (0, -14).withHeight (14), juce::Justification::centredLeft);
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
        c.removeFromTop (4);
        auto re = c.removeFromTop (30);
        exiIfxToggle.setBounds (re.removeFromLeft (300));
        exiVelLabel.setBounds (re.removeFromLeft (80));
        exiVelBox.setBounds (re.removeFromLeft (360).reduced (0, 2));
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
        auto* aw = new juce::AlertWindow (juce::String::fromUTF8 ("Zvuk sa druge klavijature"),
            juce::String::fromUTF8 ("Bilo koji MIDI instrument. U koraku 3 izaberi MIDI izlaz ka tom instrumentu, njegov MIDI kanal i audio ulaz na koji je povezan.\n"
                                    "Bank / Program ostavi prazno ako zvuk biraš sam na instrumentu."), juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", "", "Ime zvuka:");
        aw->addTextEditor ("msb", "", "Bank MSB (CC0):");
        aw->addTextEditor ("lsb", "", "Bank LSB (CC32):");
        aw->addTextEditor ("pc", "", "Program (1-128):");
        aw->addComboBox ("layers", { "1", "2", "3", "4", "6", "8" }, juce::String::fromUTF8 ("Jačine (velocity slojevi):"));
        if (auto* cb = aw->getComboBoxComponent ("layers")) cb->setSelectedItemIndex (2);
        aw->addButton ("Dodaj", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton (juce::String::fromUTF8 ("Otkaži"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
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
            n.usedBy = (n.pc >= 0 ? "PC " + juce::String (n.pc + 1) : juce::String ("bez PC")) + (n.msb >= 0 || n.lsb >= 0 ? " / bank " + juce::String (n.msb) + "-" + juce::String (n.lsb) : juce::String());
            for (auto& x : safe->needed) if (x.romKey == n.romKey) return;
            safe->needed.push_back (n);
            safe->neededList.updateContent();
            safe->neededList.repaint();
            safe->updateInfo();
        }), true);
    }

    // desni klik na listu: ponovo snimi (brise stare semplove tog programa / multisempla)
    void neededMenu (int row)
    {
        if (row < 0 || row >= (int) needed.size()) return;
        if (! neededList.isRowSelected (row)) neededList.selectRow (row);
        auto sel = neededList.getSelectedRows();
        juce::PopupMenu m;
        m.addItem (1, juce::String::fromUTF8 ("Presempluj ponovo (obriši stare semplove)"), ! isThreadRunning());
        m.addItem (2, "Ukloni sa liste", ! isThreadRunning());
        juce::Component::SafePointer<MainComponent> safe (this);
        m.showMenuAsync ({}, [safe, sel] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == 2) { safe->removeButton.triggerClick(); return; }
            juce::StringArray names;
            for (int i = 0; i < sel.size(); ++i)
                if (sel[i] < (int) safe->needed.size()) names.add (safe->needed[(size_t) sel[i]].label());
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Presempluj ponovo",
                juce::String::fromUTF8 ("Obrisaću snimljene semplove za:\n\n") + names.joinIntoString ("\n")
                + juce::String::fromUTF8 ("\n\nPosle klikni START da se snime ponovo. U Pikaso Workstation klikni Osveži (PODEŠAVANJA) kad se snimi."),
                juce::String::fromUTF8 ("Obriši"), juce::String::fromUTF8 ("Otkaži"), nullptr,
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
        fileLabel.setText (f.getFullPathName() + "   (" + juce::String ((int) pcg->programs.size()) + " programa, "
                           + juce::String ((int) pcg->combis.size()) + " kombija, MIDI kanal Kronosa " + juce::String (pcg->globalChannel) + ")",
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
            return "KOMBI " + c.id() + "  " + c.name;
        }
        const auto& p = pcg->programs[(size_t) code];
        return p.id() + "  " + p.name;
    }

    bool programHasRom (const Kronos::Program& p) const
    {
        if (p.exi) return true;
        if (! p.hd1 || p.drumKit) return false;
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
            needed.push_back (std::move (n));
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
                needed.push_back (n);
            }
    }

    void addSelected()
    {
        if (pcg == nullptr) return;
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

    void updateInfo()
    {
        int done = 0, exi = 0, seconds = 0;
        for (auto& n : needed)
        {
            if (isDone (n)) { ++done; continue; }
            if (n.exi || n.external) ++exi;
            seconds += estimateSecondsPerMs() * (n.exi || n.external ? (int) exiVelocities (n).size() : 1);
        }
        info.setText (juce::String ((int) needed.size() - exi) + " multisemplova + " + juce::String (exi) + " EXi/ostalih zvukova, gotovo " + juce::String (done)
                      + juce::String::fromUTF8 ("  (preostalo: ~") + juce::String (seconds / 60) + " min)",
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
                juce::String::fromUTF8 ("Izaberi MIDI ulaz i izlaz Kronosa."));
            return;
        }
        sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);   // Kronos u PROGRAM mod
        auto reply = sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x74, 0x00 }, 0x75, 3000);
        if (! reply)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SysEx",
                juce::String::fromUTF8 ("Kronos nije odgovorio. Proveri MIDI kanal (Global MIDI kanal Kronosa), da je Kronos u PROGRAM modu i da je u GLOBAL > MIDI uključeno primanje/slanje Exclusive poruka."));
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
                juce::String::fromUTF8 ("Trenutni program nije HD-1 program (") + juce::String ((int) dec.size())
                + juce::String::fromUTF8 (" bajtova). Na Kronosu izaberi Init Program (HD-1, npr. USER-FF 006) i probaj ponovo."));
            return;
        }
        sxStart = best;
        sxShift = bestShift;
        sxHeader.assign (r.begin(), r.begin() + best);
        sxHeader[3] = 0x75;
        sxTemplate = unpack7 (r.data() + best, (int) r.size() - best);
        juce::String nm;
        for (int k = 0; k < 24 && sxTemplate[(size_t) (sxShift + k)] != 0; ++k) nm << (juce::juce_wchar) sxTemplate[(size_t) (sxShift + k)];
        templateInfo.setText (juce::String::fromUTF8 ("Šablon sa Kronosa: ") + nm.trim() + " (" + juce::String ((int) sxTemplate.size()) + juce::String::fromUTF8 (" bajtova)"),
                              juce::dontSendNotification);
        outFolder.createDirectory();
        outFolder.getChildFile ("Kronos-program.syx").replaceWithData (r.data(), r.size());
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
                setStatus ("Kronos javlja grešku " + juce::String::toHexString ((int) (*reply)[5]));
        }
        return true;
    }

    // jacine za EXi program (velocity slojevi)
    std::vector<int> exiVelocities (const NeededMs& n) const
    {
        int layers = n.external ? n.layers : exiVelBox.getSelectedId() - 100;
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
        if (midiIn == nullptr || midi == nullptr) { err = juce::String::fromUTF8 ("Za EXi programe izaberi MIDI ulaz i izlaz Kronosa."); return false; }
        sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);
        auto r = sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x74, 0x00 }, 0x75, 3000);
        if (! r || r->size() < 8)
        {
            err = juce::String::fromUTF8 ("Kronos nije odgovorio na SysEx. Proveri MIDI kanal, PROGRAM mod i GLOBAL > MIDI > Exclusive.");
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
            err = "Kronos javlja grešku " + juce::String::toHexString ((int) (*reply)[5]) + juce::String::fromUTF8 (" za EXi program (") + n.programId + ")";
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
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "PCG", juce::String::fromUTF8 ("Ne mogu da upišem ") + outFile.getFullPathName());
            return;
        }
        outFile.withFileExtension ("txt").replaceWithText (mapLines.joinIntoString ("\n"));
        neededList.repaint();
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "PCG",
            juce::String::fromUTF8 ("Napravljen ") + outFile.getFileName() + ": " + juce::String (written) + " programa u banci "
            + Kronos::programBankName (bankId) + juce::String::fromUTF8 (" od broja ") + juce::String ((int) startNum.getValue())
            + (written < (int) needed.size() ? juce::String::fromUTF8 ("\nNisu svi stali u banku: posle snimanja napravi sledeći PCG za ostatak.") : juce::String())
            + juce::String::fromUTF8 ("\n\nNa Kronosu: Disk → Load → izaberi ovaj PCG i učitaj samo banku ") + Kronos::programBankName (bankId) + ".");
    }

    //==========================================================================
    void showAudioSettings()
    {
        auto* sel = new juce::AudioDeviceSelectorComponent (deviceManager, 1, 2, 0, 2, false, false, true, false);
        sel->setSize (520, 380);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (sel);
        o.dialogTitle = "Audio ulaz";
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
        chooser = std::make_unique<juce::FileChooser> ("Folder za semplove", outFolder);
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
        auto nm = n.label() + " " + n.usedBy;
        return outFolder.getChildFile (juce::File::createLegalFileName (nm.trim()));
    }

    // gotovo samo ako je snimljen bas ovaj program (EXi program izmenjen na Kronosu ima drugi kljuc)
    bool isDone (const NeededMs& n) const
    {
        const auto tag = msFolder (n).getChildFile ("kronos-ms.txt");
        if (! tag.existsAsFile()) return false;
        return juce::StringArray::fromLines (tag.loadFileAsString())[0].trim().equalsIgnoreCase (n.romKey);
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
                juce::String::fromUTF8 ("Izaberi MIDI izlaz ka Kronosu i audio ulaz (Kronos USB)."));
            return;
        }
        if (outFolder == juce::File())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Izaberi folder za semplove (dugme \"Folder za semplove...\"). Neka bude u folderu biblioteke Pikaso Workstation, npr. C:\\KRONOS\\KRONOS ROM."));
            return;
        }
        useSysex = sysexMode.getToggleState();
        bool anyRom = false, anyExi = false;
        for (auto& n : needed) { anyRom = anyRom || (! n.exi && ! n.external && ! isDone (n)); anyExi = anyExi || (n.exi && ! isDone (n)); }
        if (anyExi && midiIn == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("EXi programi se šalju preko SysEx-a: izaberi i MIDI ulaz sa Kronosa."));
            return;
        }
        if (useSysex && anyRom && sxTemplate.empty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Na Kronosu izaberi Init Program (npr. USER-FF 006) pa klikni \"Pročitaj šablon sa Kronosa\"."));
            return;
        }
        bool anySlot = useSysex || ! anyRom;
        for (auto& n : needed) anySlot = anySlot || n.slot >= 0;
        if (! anySlot)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Prvo napravi PCG (korak 2) i učitaj ga na Kronos."));
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
        exiVels.clear();
        for (auto& n : needed) exiVels.push_back (n.exi || n.external ? exiVelocities (n) : std::vector<int> {});
        outFolder.createDirectory();
        saveSettings();
        analyzeOnly = false;
        autoAnalyze = autoAnalyzeToggle.getToggleState();
        startButton.setButtonText ("STOP");
        startThread();
    }

    // procena izabranih zvukova (ili svih ako nista nije izabrano)
    void startAnalysis()
    {
        if (isThreadRunning() || needed.empty()) return;
        if (midi == nullptr || deviceManager.getCurrentAudioDevice() == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Izaberi MIDI izlaz i audio ulaz."));
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
                juce::String::fromUTF8 ("Za ROM multisemplove prvo klikni \"Pročitaj šablon sa Kronosa\"."));
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
                      bool autoLoop = true; double xfadeMs = 80, minLoopMs = 300; } settings;

    void setStatus (const juce::String& s)
    {
        const juce::ScopedLock sl (statusLock);
        statusText = s;
    }

    // izabere zvuk za posao: druga klavijatura (Program Change), EXi (SysEx), ROM multisempl (SysEx ili PCG)
    // Kronos: edit buffer prima samo program istog tipa kao banka u kojoj je (EXi banka ili HD-1 banka),
    // inace javlja "bank conflict". Zato se pre slanja izabere program odgovarajuceg tipa (Program Change).
    int lastKind = 0;   // 0 nepoznato, 1 HD-1, 2 EXi

    void selectKronosProgram (juce::uint32 bankId, int number)
    {
        // Kronos bank select za programe: CC0 = 0, CC32: INT-A..F = 0..5, USER-A..G = 8..14, USER-AA..GG = 15..21
        const int hi = (int) (bankId >> 16), lo = (int) (bankId & 0xffff);
        int lsb = -1;
        if (hi == 0 && lo <= 5) lsb = lo;
        if (hi == 2 && lo <= 13) lsb = 8 + lo;
        if (lsb < 0 || number < 0 || number > 127) return;
        const int ch = settings.channel;
        sysexCall ({ 0x42, sysexChannelByte(), 0x68, 0x4E, 0x02 }, 0x24, 800);   // PROGRAM mod
        send (juce::MidiMessage::controllerEvent (ch, 0, 0));
        send (juce::MidiMessage::controllerEvent (ch, 32, lsb));
        send (juce::MidiMessage::programChange (ch, number));
        wait (700);
    }

    void ensureKronosKind (const NeededMs& n)
    {
        const int kind = n.exi ? 2 : 1;
        if (kind == lastKind || pcg == nullptr) return;
        if (n.exi) selectKronosProgram (n.progBank, n.progNum);
        else
        {
            // HD-1 program: "Init..." iz PCG-a, inace prvi HD-1 program
            const Kronos::Program* pick = nullptr;
            for (auto& p : pcg->programs)
                if (p.hd1 && ! p.drumKit && (pick == nullptr || (p.name.startsWithIgnoreCase ("init") && ! pick->name.startsWithIgnoreCase ("init"))))
                    pick = &p;
            if (pick != nullptr) selectKronosProgram (pick->bankId, pick->number);
        }
        lastKind = kind;
    }

    bool selectJob (const NeededMs& n, juce::String& err, bool& fatal)
    {
        fatal = false;
        if (! n.external) ensureKronosKind (n);
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
            if (! sendProgramSysex (n)) { err = juce::String::fromUTF8 ("Šablon nije pročitan"); fatal = true; return false; }
        }
        else
        {
            if (n.slot < 0) { err = "nema programa u PCG-u"; return false; }
            selectProgram (n.slot);
        }
        wait (900);
        return true;
    }

    void run() override
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
                if (i < 0 || i >= total) continue;
                auto n = needed[(size_t) i];
                juce::String err; bool fatal = false;
                setStatus ("Procena " + juce::String ((int) k + 1) + "/" + juce::String ((int) analyzeRows.size()) + "  " + n.label());
                if (! selectJob (n, err, fatal))
                {
                    n.anText = juce::String::fromUTF8 ("procena nije uspela: ") + err;
                    storeAnalysis (i, n);
                    lastErr = err;
                    if (fatal) break;
                    continue;
                }
                analyzeJob (n);
                storeAnalysis (i, n);
                if (n.analyzed) ++ok;
            }
            send (juce::MidiMessage::allNotesOff (settings.channel));
            if (threadShouldExit()) setStatus ("Zaustavljeno.");
            else if (ok == (int) analyzeRows.size())
                setStatus (juce::String::fromUTF8 ("Procena gotova: vrednosti su u listi desno, pored imena zvuka, i koriste se pri snimanju."));
            else
                setStatus (juce::String::fromUTF8 ("Procena: uspelo ") + juce::String (ok) + " od " + juce::String ((int) analyzeRows.size())
                           + (lastErr.isNotEmpty() ? " (" + lastErr + ")" : juce::String::fromUTF8 (" - ništa se ne čuje: proveri audio ulaz (Kronos USB), MIDI kanal i da li je ULAZ merač pomeren")));
            return;
        }

        for (int i = 0; i < total && ! threadShouldExit(); ++i)
        {
            auto n = needed[(size_t) i];
            if (isDone (n)) continue;
            setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  (" + n.usedBy + ")");
            juce::String err; bool fatal = false;
            if (! selectJob (n, err, fatal)) { setStatus (err); if (fatal) break; continue; }
            if (autoAnalyze && ! n.analyzed)
            {
                setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  procena...");
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

            if (n.exi || n.external)
            {
                recordExi (n, i, total, exiVels[(size_t) i]);
            }
            else
            {
                const auto folder = msFolder (n);
                folder.createDirectory();
                int written = 0;
                for (int note = settings.low; note <= settings.high && ! threadShouldExit(); note += settings.step)
                {
                    setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  nota " + noteName (note));
                    if (recordNote (note, folder.getChildFile (n.label().replaceCharacter (' ', '_') + " " + juce::String (note).paddedLeft ('0', 3) + ".wav")))
                        ++written;
                }
                if (! threadShouldExit())
                {
                    if (written > 0)
                        folder.getChildFile ("kronos-ms.txt").replaceWithText (n.romKey + "\n" + n.label() + "\n" + n.usedBy + "\n");
                    else
                        folder.deleteRecursively();   // nista se ne cuje (npr. EXs nije instaliran)
                }
            }
            settings = saved;
            if (threadShouldExit()) break;
            juce::MessageManager::callAsync ([this] { neededList.repaint(); updateInfo(); });
        }
        send (juce::MidiMessage::allNotesOff (settings.channel));
        setStatus (threadShouldExit() ? "Zaustavljeno." : juce::String::fromUTF8 ("Gotovo. U Pikaso Workstation klikni Osveži (PODEŠAVANJA) pa ponovo napravi usere od Kronos programa."));
    }

    void storeAnalysis (int i, const NeededMs& n)
    {
        const auto key = n.romKey;
        const bool ok = n.analyzed, loop = n.anLoop;
        const double hold = n.anHold, rel = n.anRelease;
        const auto text = n.anText;
        juce::MessageManager::callAsync ([this, i, key, ok, loop, hold, rel, text]
        {
            if (i < (int) needed.size() && needed[(size_t) i].romKey == key)
            {
                auto& m = needed[(size_t) i];
                m.analyzed = ok; m.anLoop = loop; m.anHold = hold; m.anRelease = rel; m.anText = text;
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
        return recorder.writePos.load();
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
            setStatus ("Procena: " + n.label() + "  nota " + noteName (note));
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
            const bool sustained = dropDb > -15.0 && slope > -3.0;
            double hold;
            if (sustained)
            {
                ++sustainedCount;
                // od kad se zvuk smirio (ostaje unutar 3 dB od kraja)
                int settle = offB - 1;
                while (settle > peakB && std::abs (20.0 * std::log10 (env[(size_t) settle] / endHold)) < 3.0) --settle;
                hold = juce::jlimit (2.5, 10.0, (settle - onset) * 0.01 + 2.0);
            }
            else
            {
                int end = peakB;
                while (end < offB && env[(size_t) end] > peak * 0.003) ++end;   // -50 dB
                hold = end < offB ? (end - onset) * 0.01 + 0.3 : testHold;
            }
            // release: od pustanja tastera do -60 dB (ili do suma)
            double floorLvl = 1.0e-5;
            if (onset > 3) floorLvl = juce::jmax (floorLvl, avg (0, onset) * 2.0);
            const double startRel = env[(size_t) juce::jmax (0, offB - 1)];
            int r = offB;
            while (r < nb && env[(size_t) r] > juce::jmax (startRel * 0.001, floorLvl)) ++r;
            const double rel = startRel > floorLvl * 2.0 ? (r - offB) * 0.01 : 0.0;
            holdNeed = juce::jmax (holdNeed, hold);
            relNeed = juce::jmax (relNeed, rel);
        }
        if (heard == 0)
        {
            n.analyzed = false;
            n.anText = juce::String::fromUTF8 ("procena: ništa se ne čuje (audio ulaz / MIDI kanal?)");
            return;
        }
        n.analyzed = true;
        n.anLoop = sustainedCount * 2 > heard;
        n.anHold = std::round (holdNeed * 10.0) / 10.0;
        n.anRelease = juce::jlimit (0.3, 15.0, std::round ((relNeed + 0.3) * 10.0) / 10.0);
        n.anText = juce::String::fromUTF8 ("drži ") + juce::String (n.anHold, 1) + " s, release " + juce::String (n.anRelease, 1) + " s, "
                 + (n.anLoop ? "loop" : juce::String::fromUTF8 ("bez loop-a (zvuk opada)"));
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
                setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  nota " + noteName (note)
                           + juce::String::fromUTF8 ("  jačina ") + juce::String (vel));
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
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (file);
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
        auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)
                                                   .withMetadataValues (meta));
        if (writer == nullptr) return false;
        writer->writeFromAudioSampleBuffer (outBuf, 0, outLen);
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
    juce::TextButton externalButton, analyzeButton;
    juce::ToggleButton autoAnalyzeToggle;
    bool analyzeOnly = false, autoAnalyze = true;
    std::vector<int> analyzeRows;
    juce::TextButton loadButton, addButton, clearButton, removeButton, makePcgButton, audioButton, folderButton, testButton, startButton;
    juce::ToggleButton showCombis;
    juce::TextEditor search;
    juce::ListBox sourceList, neededList;
    juce::ComboBox templateBox, bankBox, midiOut, midiInBox;
    juce::ToggleButton sysexMode, autoLoopToggle, exiIfxToggle;
    juce::Label exiVelLabel;
    juce::ComboBox exiVelBox;
    std::vector<std::vector<int>> exiVels;
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
        auto f = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("Pikaso Autosampler GRESKA.txt");
        f.replaceWithText ("Pikaso Autosampler se srusio.\n\n" + juce::SystemStats::getStackBacktrace());
    }

    void initialise (const juce::String& commandLine) override
    {
        juce::SystemStats::setApplicationCrashHandler (crashHandler);
        logger.reset (juce::FileLogger::createDefaultAppLogger ("LiveSampler", "autosampler-log.txt", "Pikaso Autosampler"));
        juce::Logger::setCurrentLogger (logger.get());
        juce::Logger::writeToLog ("start: " + commandLine);
        juce::LookAndFeel::getDefaultLookAndFeel().setColour (juce::ResizableWindow::backgroundColourId, bg);
        window = std::make_unique<Window>();
        juce::Logger::writeToLog ("prozor napravljen");
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

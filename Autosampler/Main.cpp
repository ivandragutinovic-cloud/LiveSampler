// Kronos Autosampler: presempluje fabricke ROM / EXs multisemplove koje koriste izabrani programi i kombiji.
// 1) iz PCG-a nadje koje ROM multisemplove trebaju
// 2) napravi PCG sa po jednim programom za svaki multisempl (OSC1 MS1 = taj multisempl, bez efekata)
// 3) preko MIDI-ja bira te programe na Kronosu, svira svaki treci ton i snima preko USB audio ulaza
// 4) cuva WAV fajlove + kronos-ms.txt, pa ih Live Sampler koristi umesto ROM zvuka.
#include <JuceHeader.h>
#include "KronosPcg.h"
#include "RomNames.h"
#include <optional>

namespace
{
    const juce::Colour bg (0xff0f1115), panel (0xff1a1d24), accent (0xffff8a3d), dim (0xff8a93a6), lcd (0xff8fe9ff);

    juce::String noteName (int n) { return juce::MidiMessage::getMidiNoteName (n, true, true, 3); }

    // sirovi pristup programima u PCG-u (za citanje sablona i upis novih programa)
    struct RawBank { juce::uint32 bankId = 0; size_t firstItem = 0; int count = 0, itemSize = 0; };

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
                else if (id == "PBK1" && size >= 12)
                {
                    RawBank b;
                    b.count = (int) be32 (content);
                    b.itemSize = (int) be32 (content + 4);
                    b.bankId = be32 (content + 8);
                    b.firstItem = content + 12;
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
        juce::String label() const
        {
            const int type = zone[0], bank = zone[16], index = zone[18] | (zone[19] << 8);
            juce::String s = type == 2 ? "WaveSeq-" + juce::String (bank) : (bank == 0 ? "ROM" : "EXs-" + juce::String (bank));
            s << " " << juce::String (index).paddedLeft ('0', 4);
            if (bank == 0 && type == 1 && index < kNumRomMsNames && kRomMsNames[index][0] != 0)
                s << " " << kRomMsNames[index];
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
        const auto err = deviceManager.initialise (2, 2, nullptr, true);
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
        styleTitle (title2, juce::String::fromUTF8 ("2. PROGRAM ZA SVAKI MULTISEMPL: direktno preko SysEx-a (preporučeno) ili preko PCG-a"));
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
        addAndMakeVisible (makePcgButton);

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
        addAndMakeVisible (sysexMode);
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

        folderButton.setButtonText (juce::String::fromUTF8 ("Folder za semplove..."));
        folderButton.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (folderButton);
        folderLabel.setColour (juce::Label::textColourId, lcd);
        addAndMakeVisible (folderLabel);

        testButton.setButtonText ("Test (prvi multisempl)");
        testButton.onClick = [this]
        {
            if (needed.empty()) return;
            if (sysexMode.getToggleState()) { sendProgramSysex (needed[0]); send (juce::MidiMessage::noteOn ((int) channel.getValue(), 60, (juce::uint8) 127)); juce::Thread::sleep (800); send (juce::MidiMessage::noteOff ((int) channel.getValue(), 60)); }
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
            "Redosled: 1) učitaj PCG i dodaj programe/kombije  2) Napravi PCG, pa ga na Kronosu učitaj u izabranu banku "
            "(Load PCG → samo tu banku)  3) izaberi Kronos USB audio ulaz i MIDI izlaz, podesi Bank CC0/CC32 tako da Test "
            "na Kronosu izabere prvi novi program, pa START. Snimanje se može prekinuti i nastaviti: postojeći fajlovi se preskaču."),
            juce::dontSendNotification);
        help.setColour (juce::Label::textColourId, dim);
        help.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (help);

        startTimerHz (10);
        updateInfo();
        setSize (1180, 820);
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
        stopThread (4000);
        if (midiIn != nullptr) midiIn->stop();
        deviceManager.removeAudioCallback (&recorder);
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
        sec2 = r.removeFromTop (118);
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
        auto right = a;
        auto infoRow = right.removeFromBottom (28);
        removeButton.setBounds (infoRow.removeFromRight (130));
        infoRow.removeFromRight (6);
        clearButton.setBounds (infoRow.removeFromRight (110));
        info.setBounds (infoRow);
        neededList.setBounds (right.withTrimmedTop (32));

        auto b = sec2.reduced (10);
        title2.setBounds (b.removeFromTop (22));
        auto row = b.removeFromTop (32);
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
        sysexMode.setBounds (rowS.removeFromLeft (520));
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
        auto r3 = c.removeFromTop (32);
        folderButton.setBounds (r3.removeFromLeft (180));
        r3.removeFromLeft (10);
        folderLabel.setBounds (r3);
        c.removeFromTop (8);
        auto r4 = c.removeFromTop (40);
        testButton.setBounds (r4.removeFromLeft (220));
        r4.removeFromLeft (10);
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
            t << "   (" << n.usedBy << ")";
            g.drawText (t, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
        }
    } neededModel;

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
        channel.setValue (pcg->globalChannel);
        if (outFolder == juce::File())
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
            if ((b.bankId >> 16) == 2)
                bankBox.addItem (Kronos::programBankName (b.bankId), (int) b.bankId + 1);
        bankBox.setSelectedItemIndex (juce::jmax (0, bankBox.getNumItems() - 1));
        rebuildSource();
    }

    juce::String sourceText (int code) const
    {
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
        int done = 0;
        for (auto& n : needed) if (isDone (n)) ++done;
        info.setText (juce::String ((int) needed.size()) + " multisemplova, presemplovano " + juce::String (done)
                      + juce::String::fromUTF8 ("  (procena: ~") + juce::String ((int) needed.size() * estimateSecondsPerMs() / 60) + " min)",
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
            if (b.bankId == p.bankId && p.number < b.count)
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
            setStatus ("Kronos javlja grešku " + juce::String::toHexString ((int) (*reply)[5]));
        return true;
    }

    //==========================================================================
    void makePcg()
    {
        if (pcg == nullptr || needed.empty()) return;
        const int tIdx = templateBox.getSelectedId() - 1;
        const juce::uint32 bankId = (juce::uint32) (bankBox.getSelectedId() - 1);
        const RawBank* target = nullptr;
        for (auto& b : banks) if (b.bankId == bankId) target = &b;
        if (tIdx < 0 || target == nullptr) return;

        // sablon: sirovi bajtovi izabranog programa
        const auto& tp = pcg->programs[(size_t) tIdx];
        const RawBank* tb = nullptr;
        for (auto& b : banks) if (b.bankId == tp.bankId) tb = &b;
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
        auto nm = n.label() + " " + n.usedBy;
        return outFolder.getChildFile (juce::File::createLegalFileName (nm.trim()));
    }

    bool isDone (const NeededMs& n) const { return msFolder (n).getChildFile ("kronos-ms.txt").existsAsFile(); }

    //==========================================================================
    void startSampling()
    {
        if (midi == nullptr || deviceManager.getCurrentAudioDevice() == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Izaberi MIDI izlaz ka Kronosu i audio ulaz (Kronos USB)."));
            return;
        }
        useSysex = sysexMode.getToggleState();
        if (useSysex && sxTemplate.empty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Autosampler",
                juce::String::fromUTF8 ("Na Kronosu izaberi Init Program (npr. USER-FF 006) pa klikni \"Pročitaj šablon sa Kronosa\"."));
            return;
        }
        bool anySlot = useSysex;
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
        outFolder.createDirectory();
        startButton.setButtonText ("STOP");
        startThread();
    }

    struct Settings { int channel = 1, low = 24, high = 108, step = 3, velocity = 127; double hold = 6, release = 3; } settings;

    void setStatus (const juce::String& s)
    {
        const juce::ScopedLock sl (statusLock);
        statusText = s;
    }

    void run() override
    {
        const int total = (int) needed.size();
        for (int i = 0; i < total && ! threadShouldExit(); ++i)
        {
            auto n = needed[(size_t) i];
            if (isDone (n)) continue;
            setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  (" + n.usedBy + ")");
            if (useSysex)
            {
                if (! sendProgramSysex (n)) break;
            }
            else
            {
                if (n.slot < 0) continue;
                selectProgram (n.slot);
            }
            wait (900);

            const auto folder = msFolder (n);
            folder.createDirectory();
            int written = 0;
            for (int note = settings.low; note <= settings.high && ! threadShouldExit(); note += settings.step)
            {
                setStatus (juce::String (i + 1) + "/" + juce::String (total) + "  " + n.label() + "  nota " + noteName (note));
                if (recordNote (note, folder.getChildFile (n.label().replaceCharacter (' ', '_') + " " + juce::String (note).paddedLeft ('0', 3) + ".wav")))
                    ++written;
            }
            if (threadShouldExit()) break;
            if (written > 0)
                folder.getChildFile ("kronos-ms.txt").replaceWithText (n.romKey + "\n" + n.label() + "\n" + n.usedBy + "\n");
            else
                folder.deleteRecursively();   // nista se ne cuje (npr. EXs nije instaliran)
            juce::MessageManager::callAsync ([this] { neededList.repaint(); updateInfo(); });
        }
        send (juce::MidiMessage::allNotesOff (settings.channel));
        setStatus (threadShouldExit() ? "Zaustavljeno." : juce::String::fromUTF8 ("Gotovo. U Live Sampleru klikni Osveži (PODEŠAVANJA) pa ponovo napravi usere od Kronos programa."));
    }

    bool recordNote (int note, const juce::File& file)
    {
        const double sr = recorder.sampleRate;
        // sacekaj tisinu pre note
        for (int k = 0; k < 40 && recorder.level.load() > 0.002f && ! threadShouldExit(); ++k) wait (50);

        recorder.writePos = 0;
        recorder.recording = true;
        wait (40);
        send (juce::MidiMessage::noteOn (settings.channel, note, (juce::uint8) settings.velocity));
        wait ((int) (settings.hold * 1000.0));
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
        const float thr = juce::jmax (0.0003f, peak * 0.002f);
        int start = 0;
        while (start < len && std::abs (buf.getSample (0, start)) < thr && std::abs (buf.getSample (1, start)) < thr) ++start;
        start = juce::jmax (0, start - (int) (sr * 0.002));
        int end = len;
        const float endThr = peak * 0.0003f;
        while (end > start + 64 && std::abs (buf.getSample (0, end - 1)) < endThr && std::abs (buf.getSample (1, end - 1)) < endThr) --end;
        const int outLen = end - start;
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
        auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
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
    juce::TextButton loadButton, addButton, clearButton, removeButton, makePcgButton, audioButton, folderButton, testButton, startButton;
    juce::ToggleButton showCombis;
    juce::TextEditor search;
    juce::ListBox sourceList, neededList;
    juce::ComboBox templateBox, bankBox, midiOut, midiInBox;
    juce::ToggleButton sysexMode;
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
    const juce::String getApplicationName() override { return "Kronos Autosampler"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    static void crashHandler (void*)
    {
        auto f = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("Kronos Autosampler GRESKA.txt");
        f.replaceWithText ("Kronos Autosampler se srusio.\n\n" + juce::SystemStats::getStackBacktrace());
    }

    void initialise (const juce::String& commandLine) override
    {
        juce::SystemStats::setApplicationCrashHandler (crashHandler);
        logger.reset (juce::FileLogger::createDefaultAppLogger ("LiveSampler", "autosampler-log.txt", "Kronos Autosampler"));
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
        Window() : juce::DocumentWindow ("Kronos Autosampler", bg, juce::DocumentWindow::allButtons)
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

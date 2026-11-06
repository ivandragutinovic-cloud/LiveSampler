#include "Cleanup.h"
#include "Text.h"
#include <set>

namespace
{
    juce::String sizeText (juce::int64 b)
    {
        if (b < 0) return "...";
        if (b >= (juce::int64) 1024 * 1024 * 1024) return juce::String ((double) b / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
        return juce::String ((double) b / (1024.0 * 1024.0), 1) + " MB";
    }

    // presemplovan Kronos zvuk: Autosampler upise kronos-ms.txt sa "rom:" / "exi:" / "drum:"
    juce::String kronosKind (const juce::File& folder)
    {
        const auto tag = folder.getChildFile ("kronos-ms.txt");
        if (! tag.existsAsFile()) return {};
        for (auto& l : juce::StringArray::fromLines (tag.loadFileAsString()))
        {
            const auto t = l.trim().toLowerCase();
            if (t.startsWith ("exi:")) return "EXi";
            if (t.startsWith ("drum:")) return "DRUM";
            if (t.startsWith ("rom:")) return t.contains ("exs") ? "EXs" : "ROM";
        }
        return {};
    }
}

CleanupPanel::CleanupPanel (LiveSamplerProcessor& p) : juce::Thread ("cleanup sizes"), proc (p)
{
    title.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, Pal::text);
    addAndMakeVisible (title);
    summary.setColour (juce::Label::textColourId, Pal::accent);
    summary.setFont (juce::FontOptions (13.5f));
    addAndMakeVisible (summary);
    hint.setColour (juce::Label::textColourId, Pal::dim);
    hint.setFont (juce::FontOptions (12.5f));
    hint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (hint);

    samplesTab.setButtonText (T ("UNUSED ROM / EXi SAMPLES"));
    usersTab.setButtonText (T ("UNUSED USERS"));
    for (auto* b : { &samplesTab, &usersTab })
    {
        b->setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        addAndMakeVisible (b);
    }
    samplesTab.onClick = [this] { usersMode = false; rebuild(); };
    usersTab.onClick   = [this] { usersMode = true;  rebuild(); };

    allButton.setButtonText (T ("Select all"));
    noneButton.setButtonText (T ("Select none"));
    rescanButton.setButtonText (T ("Refresh"));
    allButton.onClick  = [this] { { const juce::ScopedLock sl (itemsLock); for (auto& it : items) it.checked = true; }  list.repaint(); updateSummary(); };
    noneButton.onClick = [this] { { const juce::ScopedLock sl (itemsLock); for (auto& it : items) it.checked = false; } list.repaint(); updateSummary(); };
    rescanButton.onClick = [this] { rebuild(); };
    deleteButton.setColour (juce::TextButton::buttonColourId, Pal::red.withAlpha (0.85f));
    deleteButton.onClick = [this] { deleteSelected(); };
    for (auto* b : { &allButton, &noneButton, &rescanButton, &deleteButton }) addAndMakeVisible (b);

    list.setRowHeight (30);
    list.setColour (juce::ListBox::backgroundColourId, Pal::bg);
    list.setOutlineThickness (1);
    list.setColour (juce::ListBox::outlineColourId, Pal::panelHi);
    addAndMakeVisible (list);

    setSize (820, 640);
    rebuild();
    startTimer (100);
}

CleanupPanel::~CleanupPanel()
{
    stopThread (3000);
    stopPlay();
}

void CleanupPanel::show (LiveSamplerProcessor& p)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new CleanupPanel (p));
    o.dialogTitle = T ("Cleanup");
    o.dialogBackgroundColour = Pal::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = true;
    o.launchAsync();
}

void CleanupPanel::rebuild()
{
    stopThread (3000);
    stopPlay();
    samplesTab.setToggleState (! usersMode, juce::dontSendNotification);
    usersTab.setToggleState (usersMode, juce::dontSendNotification);
    std::vector<Item> v;
    if (! usersMode)
    {
        title.setText (T ("Resampled Kronos sounds (ROM / EXi / drum) that no user uses"), juce::dontSendNotification);
        hint.setText (T ("Tick only what you want to delete. \xe2\x96\xb6 plays the sample (middle key, loudest layer). "
                         "Deleted folders go to the Recycle Bin, so they can still be restored. "
                         "Check the UNUSED USERS tab first: a sound used only by an unused user is not listed here until you delete that user."),
                      juce::dontSendNotification);
        std::set<juce::String> used;
        for (int u = 0; u < proc.getNumUsers(); ++u)
            for (auto& o : proc.getUser (u).osc)
                if (o.instrument.isNotEmpty()) used.insert (o.instrument);
        for (auto& name : proc.getInstrumentList())
        {
            if (used.count (name) > 0) continue;
            const auto f = proc.getInstrumentFile (name);
            if (! f.isDirectory()) continue;
            const auto kind = kronosKind (f);
            if (kind.isEmpty()) continue;      // samo presemplovani Kronos zvuci, ne Kontakt i ostale biblioteke
            Item it;
            it.name = f.getFileName();
            it.kind = kind;
            it.folder = f;
            v.push_back (it);
        }
        std::sort (v.begin(), v.end(), [] (const Item& a, const Item& b) { return a.name.compareNatural (b.name) < 0; });
        deleteButton.setButtonText (T ("DELETE TICKED (to Recycle Bin)"));
    }
    else
    {
        title.setText (T ("Users that are not in any saved performance"), juce::dontSendNotification);
        hint.setText (T ("Tick only what you want to delete. \xe2\x96\xb6 plays the user (C4, then you can play it on the keyboard until you stop it). "
                         "After deleting users, their samples may show up in the samples tab."), juce::dontSendNotification);
        for (int u = 0; u < proc.getNumUsers(); ++u)
        {
            const auto& usr = proc.getUser (u);
            if (! proc.userUsage (usr.name).isEmpty()) continue;
            Item it;
            it.name = usr.name;
            it.userIndex = u;
            it.kind = juce::String (u + 1).paddedLeft ('0', 3);
            juce::StringArray inst;
            for (auto& o : usr.osc) if (o.on && o.instrument.isNotEmpty()) inst.addIfNotAlreadyThere (o.instrument.fromLastOccurrenceOf ("/", false, false));
            it.info = (usr.category.isNotEmpty() ? usr.category + "  |  " : juce::String()) + inst.joinIntoString (", ");
            it.bytes = 0;
            v.push_back (it);
        }
        deleteButton.setButtonText (T ("DELETE TICKED USERS"));
    }
    {
        const juce::ScopedLock sl (itemsLock);
        items = std::move (v);
    }
    list.updateContent();
    list.repaint();
    updateSummary();
    if (! usersMode) startThread();
}

void CleanupPanel::run()
{
    for (size_t i = 0;; ++i)
    {
        juce::File f;
        {
            const juce::ScopedLock sl (itemsLock);
            if (i >= items.size()) break;
            f = items[i].folder;
        }
        juce::int64 total = 0;
        for (const auto& e : juce::RangedDirectoryIterator (f, true, "*", juce::File::findFiles))
        {
            if (threadShouldExit()) return;
            total += e.getFileSize();
        }
        const juce::ScopedLock sl (itemsLock);
        if (i < items.size() && items[i].folder == f) items[i].bytes = total;
    }
}

void CleanupPanel::updateSummary()
{
    const juce::ScopedLock sl (itemsLock);
    int ticked = 0;
    juce::int64 all = 0, sel = 0;
    bool counting = false;
    for (auto& it : items)
    {
        if (it.bytes < 0) counting = true;
        all += juce::jmax ((juce::int64) 0, it.bytes);
        if (it.checked) { ++ticked; sel += juce::jmax ((juce::int64) 0, it.bytes); }
    }
    juce::String s;
    if (usersMode)
        s << items.size() << T (" unused users  |  ticked: ") << ticked;
    else
        s << items.size() << T (" unused sounds, ") << sizeText (all) << (counting ? T (" (counting...)") : juce::String())
          << T ("  |  ticked: ") << ticked << ", " << sizeText (sel);
    summary.setText (s, juce::dontSendNotification);
    deleteButton.setEnabled (ticked > 0);
}

int CleanupPanel::getNumRows()
{
    const juce::ScopedLock sl (itemsLock);
    return (int) items.size();
}

void CleanupPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool)
{
    Item it;
    {
        const juce::ScopedLock sl (itemsLock);
        if (row < 0 || row >= (int) items.size()) return;
        it = items[(size_t) row];
    }
    g.fillAll (row % 2 == 0 ? Pal::panel : Pal::panel.brighter (0.03f));
    if (it.checked) g.fillAll (Pal::red.withAlpha (0.16f));

    // kvacica
    auto box = juce::Rectangle<float> (8.0f, (float) h * 0.5f - 8.0f, 16.0f, 16.0f);
    g.setColour (it.checked ? Pal::red : Pal::dim);
    g.drawRoundedRectangle (box, 3.0f, 1.5f);
    if (it.checked)
    {
        juce::Path tick;
        tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
        tick.lineTo (box.getX() + 7.0f, box.getBottom() - 4.0f);
        tick.lineTo (box.getRight() - 3.0f, box.getY() + 3.5f);
        g.strokePath (tick, juce::PathStrokeType (2.2f));
    }
    // play
    auto play = juce::Rectangle<float> (34.0f, 4.0f, 30.0f, (float) h - 8.0f);
    g.setColour (playing == row ? Pal::accent : Pal::panelHi);
    g.fillRoundedRectangle (play, 4.0f);
    g.setColour (playing == row ? juce::Colours::black : Pal::text);
    if (playing == row) g.fillRect (play.withSizeKeepingCentre (9.0f, 9.0f));
    else
    {
        juce::Path tri;
        const auto c = play.getCentre();
        tri.addTriangle (c.x - 4.0f, c.y - 6.0f, c.x - 4.0f, c.y + 6.0f, c.x + 6.0f, c.y);
        g.fillPath (tri);
    }

    auto r = juce::Rectangle<int> (74, 0, w - 80, h);
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.setColour (Pal::dim);
    g.drawText (it.kind, r.removeFromLeft (44), juce::Justification::centredLeft);
    if (! usersMode)
    {
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (sizeText (it.bytes), r.removeFromRight (90), juce::Justification::centredRight);
    }
    g.setColour (Pal::text);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    if (it.info.isNotEmpty())
    {
        const int nw = juce::jmin (r.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)), it.name) + 12);
        g.drawText (it.name, r.removeFromLeft (nw), juce::Justification::centredLeft, true);
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (it.info, r, juce::Justification::centredLeft, true);
    }
    else
        g.drawText (it.name, r, juce::Justification::centredLeft, true);
}

void CleanupPanel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (e.x >= 30 && e.x < 68) { togglePlay (row); return; }
    {
        const juce::ScopedLock sl (itemsLock);
        if (row < 0 || row >= (int) items.size()) return;
        items[(size_t) row].checked = ! items[(size_t) row].checked;
    }
    list.repaintRow (row);
    updateSummary();
}

bool CleanupPanel::playSampleFolder (const juce::File& folder)
{
    // sempl najblizi C4, najjaci sloj
    juce::File best;
    int bestScore = -1000000;
    for (const auto& e : juce::RangedDirectoryIterator (folder, false, "*.wav", juce::File::findFiles))
    {
        int note = -1, layer = 0;
        if (! SampleLibrary::parseFileName (e.getFile().getFileNameWithoutExtension(), proc.getC3Is60(), note, layer)) continue;
        const int score = -std::abs (note - 60) * 100 + layer;
        if (score > bestScore) { bestScore = score; best = e.getFile(); }
    }
    if (best == juce::File())
        for (const auto& e : juce::RangedDirectoryIterator (folder, false, "*.wav", juce::File::findFiles)) { best = e.getFile(); break; }
    if (best == juce::File()) return false;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (best));
    if (rd == nullptr) return false;
    auto sd = std::make_shared<SampleData>();
    const int len = (int) juce::jmin (rd->lengthInSamples, (juce::int64) (rd->sampleRate * 8.0));
    sd->buffer.setSize (2, len);
    rd->read (&sd->buffer, 0, len, 0, true, true);
    if (rd->numChannels == 1) sd->buffer.copyFrom (1, 0, sd->buffer, 0, 0, len);
    sd->sampleRate = rd->sampleRate;
    sd->numFrames = len;
    sd->compact();
    proc.previewSample (sd);
    return true;
}

void CleanupPanel::togglePlay (int row)
{
    const bool same = row == playing;
    stopPlay();
    if (same) { list.repaint(); return; }
    Item it;
    {
        const juce::ScopedLock sl (itemsLock);
        if (row < 0 || row >= (int) items.size()) return;
        it = items[(size_t) row];
    }
    if (! usersMode)
    {
        if (playSampleFolder (it.folder)) playing = row;
    }
    else if (it.userIndex >= 0)
    {
        proc.setAuditionUser (it.userIndex);
        playing = row;
        noteOffAt = 0;
        // kratka pauza da se semplovi ucitaju, pa C4
        juce::Timer::callAfterDelay (350, [safe = juce::Component::SafePointer<CleanupPanel> (this), row]
        {
            if (safe == nullptr || safe->playing != row) return;
            safe->proc.keyboardState.noteOn (1, 60, 0.85f);
            safe->noteOffAt = (int) juce::Time::getMillisecondCounter() + 1500;
        });
    }
    list.repaint();
}

void CleanupPanel::stopPlay()
{
    if (playing < 0) return;
    if (usersMode)
    {
        proc.keyboardState.noteOff (1, 60, 0.0f);
        proc.setAuditionUser (-1);
    }
    proc.stopPreview();
    playing = -1;
    noteOffAt = 0;
}

void CleanupPanel::timerCallback()
{
    if (noteOffAt != 0 && (int) juce::Time::getMillisecondCounter() - noteOffAt >= 0)
    {
        proc.keyboardState.noteOff (1, 60, 0.0f);
        noteOffAt = 0;
    }
    if (! usersMode)
    {
        static int tick = 0;
        if (++tick % 5 == 0) { list.repaint(); updateSummary(); }
    }
}

void CleanupPanel::deleteSelected()
{
    std::vector<Item> sel;
    {
        const juce::ScopedLock sl (itemsLock);
        for (auto& it : items) if (it.checked) sel.push_back (it);
    }
    if (sel.empty()) return;
    stopPlay();
    juce::StringArray names;
    juce::int64 bytes = 0;
    for (auto& it : sel) { names.add (it.name); bytes += juce::jmax ((juce::int64) 0, it.bytes); }
    const auto shown = names.size() > 12 ? juce::StringArray (names.begin(), 12).joinIntoString ("\n") + "\n... (+" + juce::String (names.size() - 12) + ")"
                                         : names.joinIntoString ("\n");
    const auto msg = usersMode ? T ("Delete these users?\n\n") + shown
                               : T ("Move these sample folders to the Recycle Bin (") + sizeText (bytes) + ")?\n\n" + shown;
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("Delete"), msg, T ("DELETE"), T ("Cancel"), this,
        juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<CleanupPanel> (this), sel] (int ok)
        {
            if (ok == 0 || safe == nullptr) return;
            auto& self = *safe;
            if (self.usersMode)
            {
                // od kraja ka pocetku da se indeksi ne pomere; ime se proverava ponovo
                std::vector<int> idx;
                for (auto& it : sel)
                    if (const int u = self.proc.findUser (it.name); u >= 0 && self.proc.userUsage (it.name).isEmpty()) idx.push_back (u);
                std::sort (idx.rbegin(), idx.rend());
                for (int u : idx) self.proc.deleteUser (u);
            }
            else
            {
                juce::StringArray failed;
                for (auto& it : sel)
                {
                    // jos jednom: da ga u medjuvremenu nije uzeo neki user
                    bool nowUsed = false;
                    for (int u = 0; u < self.proc.getNumUsers() && ! nowUsed; ++u)
                        for (auto& o : self.proc.getUser (u).osc)
                            if (o.instrument.isNotEmpty() && self.proc.getInstrumentFile (o.instrument) == it.folder) nowUsed = true;
                    if (nowUsed) continue;
                    if (! it.folder.moveToTrash()) failed.add (it.name);
                }
                self.proc.rescanLibrary();
                if (! failed.isEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, T ("Delete"),
                                                            T ("Could not delete:\n") + failed.joinIntoString ("\n"));
            }
            // posle rescan-a biblioteke lista se osvezi
            juce::Timer::callAfterDelay (self.usersMode ? 50 : 1500, [safe] { if (safe != nullptr) safe->rebuild(); });
        }));
}

void CleanupPanel::paint (juce::Graphics& g)
{
    g.fillAll (Pal::panel);
}

void CleanupPanel::resized()
{
    auto r = getLocalBounds().reduced (14);
    auto tabs = r.removeFromTop (34);
    samplesTab.setBounds (tabs.removeFromLeft (260).reduced (2, 0));
    usersTab.setBounds (tabs.removeFromLeft (180).reduced (2, 0));
    rescanButton.setBounds (tabs.removeFromRight (100).reduced (2, 0));
    r.removeFromTop (8);
    title.setBounds (r.removeFromTop (26));
    hint.setBounds (r.removeFromTop (48));
    summary.setBounds (r.removeFromTop (24));
    auto bottom = r.removeFromBottom (38);
    r.removeFromBottom (8);
    list.setBounds (r);
    allButton.setBounds (bottom.removeFromLeft (110).reduced (2, 2));
    noneButton.setBounds (bottom.removeFromLeft (110).reduced (2, 2));
    deleteButton.setBounds (bottom.removeFromRight (290).reduced (2, 2));
}

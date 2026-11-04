#include "Pages.h"
#include "Text.h"
#include "MidiLearn.h"
#include "KronosNames.h"
#include <optional>

static juce::String semis (double v)
{
    const int x = (int) std::lround (v);
    return x > 0 ? "+" + juce::String (x) : juce::String (x);
}

static void styleLabel (juce::Label& l, const juce::String& text, float size = 12.5f, juce::Colour c = Pal::dim)
{
    l.setText (text, juce::dontSendNotification);
    l.setFont (juce::FontOptions (size, juce::Font::bold));
    l.setColour (juce::Label::textColourId, c);
}

// Kopiraj / nalepi izmedju usera i performansa (pamti se dok je program otvoren)
namespace Clip
{
    static std::optional<OscSettings> ms;
    struct OscCopy { SoundBlock block; std::array<OscSettings, kMsPerOsc> ms; };
    static std::optional<OscCopy> osc;
    static std::optional<PerfPart> part;
}

static void setSliderValue (juce::Slider& s, double v)
{
    if (! s.isMouseButtonDown())
        s.setValue (v, juce::dontSendNotification);
}


//==============================================================================
//  Izbor sa pretragom (user / multisempl): kucaj deo imena, klik ili Enter bira
//==============================================================================
namespace Picker
{
    struct Panel : public juce::Component, private juce::ListBoxModel
    {
        // actions: stavke koje su uvek gore (npr. "+ New user"); onPick (actionIndex ili -1, vrednost)
        Panel (juce::StringArray labelsIn, juce::StringArray valuesIn, juce::StringArray actionsIn, const juce::String& current,
               std::function<void (int, const juce::String&)> pick)
            : labels (std::move (labelsIn)), values (std::move (valuesIn)), actions (std::move (actionsIn)), onPick (std::move (pick))
        {
            search.setTextToShowWhenEmpty ("Search...", Pal::dim);
            search.setFont (juce::FontOptions (15.0f));
            search.onTextChange = [this] { filter(); };
            search.onReturnKey = [this] { if (! shown.empty()) choose (firstItemRow()); };
            search.onEscapeKey = [this] { close(); };
            addAndMakeVisible (search);
            list.setModel (this);
            list.setRowHeight (24);
            list.setColour (juce::ListBox::backgroundColourId, Pal::bg);
            addAndMakeVisible (list);
            filter();
            const int cur = values.indexOf (current);
            for (size_t r = 0; r < shown.size(); ++r)
                if (shown[r] == cur && cur >= 0) { list.selectRow ((int) r); list.scrollToEnsureRowIsOnscreen ((int) r); }
            setSize (420, 460);
        }
        void visibilityChanged() override
        {
            juce::Component::SafePointer<Panel> safe (this);
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->search.grabKeyboardFocus(); });
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (4);
            search.setBounds (r.removeFromTop (30));
            r.removeFromTop (4);
            list.setBounds (r);
        }
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::returnKey && list.getSelectedRow() >= 0) { choose (list.getSelectedRow()); return true; }
            return false;
        }
        int firstItemRow() const
        {
            for (size_t r = 0; r < shown.size(); ++r) if (shown[r] >= 0) return (int) r;
            return 0;
        }
        void filter()
        {
            shown.clear();
            const auto words = juce::StringArray::fromTokens (search.getText().trim(), " ", "");
            if (search.getText().trim().isEmpty())
                for (int a = 0; a < actions.size(); ++a) shown.push_back (-1 - a);
            for (int i = 0; i < labels.size(); ++i)
            {
                bool ok = true;
                for (auto& w : words) if (w.isNotEmpty() && ! labels[i].containsIgnoreCase (w)) { ok = false; break; }
                if (ok) shown.push_back (i);
            }
            list.updateContent();
            list.repaint();
        }
        int getNumRows() override { return (int) shown.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (row < 0 || row >= (int) shown.size()) return;
            const int i = shown[(size_t) row];
            if (selected) g.fillAll (Pal::accent.withAlpha (0.35f));
            g.setFont (juce::FontOptions (14.0f));
            g.setColour (i < 0 ? Pal::lcdText : Pal::text);
            g.drawText (i < 0 ? actions[-1 - i] : labels[i], 8, 0, w - 10, h, juce::Justification::centredLeft, true);
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override { choose (row); }
        void returnKeyPressed (int row) override { choose (row); }
        void choose (int row)
        {
            if (row < 0 || row >= (int) shown.size()) return;
            const int i = shown[(size_t) row];
            auto fn = onPick;
            const auto v = i >= 0 ? values[i] : juce::String();
            close();
            if (fn) fn (i < 0 ? -1 - i : -1, v);
        }
        void close()
        {
            if (auto* cb = findParentComponentOfClass<juce::CallOutBox>()) cb->dismiss();
        }

        juce::StringArray labels, values, actions;
        std::function<void (int, const juce::String&)> onPick;
        juce::TextEditor search;
        juce::ListBox list;
        std::vector<int> shown;
    };

    void show (juce::Component& target, juce::StringArray labels, juce::StringArray values, juce::StringArray actions,
               const juce::String& current, std::function<void (int, const juce::String&)> onPick)
    {
        auto* top = target.getTopLevelComponent();
        juce::CallOutBox::launchAsynchronously (std::make_unique<Panel> (std::move (labels), std::move (values), std::move (actions), current, std::move (onPick)),
                                                top->getLocalArea (&target, target.getLocalBounds()), top);
    }
}

//==============================================================================
//  PartStrip
//==============================================================================
PartStrip::PartStrip (LiveSamplerProcessor& p, LiveSamplerEditor& e, int i)
    : proc (p), editor (e), index (i), colour (Pal::part[i]),
      volume (T ("Volume"), Pal::part[i], -60.0, 6.0, -12.0, UI::dbText, 0.0),
      pan ("Pan", Pal::part[i], -1.0, 1.0, 0.0, UI::panText, 0.0),
      reverb ("Reverb", Pal::part[i], 0.0, 1.0, 0.0, UI::pctText, 0.2),
      delay ("Delay", Pal::part[i], 0.0, 1.0, 0.0, UI::pctText, 0.0)
{
    onButton.setButtonText ("PART " + juce::String (i + 1));
    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonOnColourId, colour);
    onButton.onClick = [this] { edit ([this] (PerfPart& pp) { pp.on = onButton.getToggleState(); }); };
    addAndMakeVisible (onButton);

    userButton.setColour (juce::TextButton::buttonColourId, Pal::bg);
    userButton.getProperties().set ("fontSize", 14.0f);
    userButton.onClick = [this] { showUserMenu(); };
    userButton.setTooltip ("Choose the user (type to search)");
    addAndMakeVisible (userButton);
    userPrev.setButtonText ("-");
    userNext.setButtonText ("+");
    userPrev.setTooltip ("Previous user (by number)");
    userNext.setTooltip ("Next user (by number)");
    userPrev.onClick = [this] { stepUser (-1); };
    userNext.onClick = [this] { stepUser (1); };
    for (auto* b : { &userPrev, &userNext }) { b->getProperties().set ("fontSize", 12.0f); addAndMakeVisible (b); }

    editButton.setButtonText ("EDIT USER");
    editButton.getProperties().set ("fontSize", 11.0f);
    editButton.onClick = [this] { editor.editUser (proc.getPerformance().parts[(size_t) index].user); };
    addAndMakeVisible (editButton);

    partMenu.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
    partMenu.setTooltip (T ("Copy / paste the whole part (user, volume, EQ, split...) into another performance"));
    partMenu.onClick = [this] { showPartMenu(); };
    addAndMakeVisible (partMenu);

    eqButton.setButtonText ("EQ");
    eqButton.getProperties().set ("fontSize", 11.0f);
    eqButton.setTooltip (T ("Adjust the EQ of this part (added to the user's EQ)"));
    eqButton.onClick = [this] { showEq(); };
    addAndMakeVisible (eqButton);

    volume.onChange = [this] (double v) { edit ([v] (PerfPart& pp) { pp.volumeDb = (float) v; }); };
    pan.onChange    = [this] (double v) { edit ([v] (PerfPart& pp) { pp.pan = (float) v; }); };
    reverb.onChange = [this] (double v) { edit ([v] (PerfPart& pp) { pp.reverbSend = (float) v; }); };
    delay.onChange  = [this] (double v) { edit ([v] (PerfPart& pp) { pp.delaySend = (float) v; }); };
    for (auto* k : { &volume, &pan, &reverb, &delay })
        addAndMakeVisible (k);

    auto c3 = [this] { return proc.getC3Is60(); };
    UI::setupIncDec (transpose, -24, 24, 1, semis);
    UI::setupIncDec (octave, -3, 3, 1, semis);
    UI::setupNoteBox (keyLow, c3);
    UI::setupNoteBox (keyHigh, c3);
    UI::setupIncDec (velLow, 1, 127, 1);
    UI::setupIncDec (velHigh, 1, 127, 1);
    UI::setupIncDec (channel, 0, 16, 1, [] (double v) { return v < 0.5 ? T ("All") : juce::String ((int) v); });

    transpose.onValueChange = [this] { edit ([this] (PerfPart& pp) { pp.transpose = (int) transpose.getValue(); }); };
    octave.onValueChange    = [this] { edit ([this] (PerfPart& pp) { pp.octave = (int) octave.getValue(); }); };
    keyLow.onValueChange    = [this] { edit ([this] (PerfPart& pp) { pp.keyLow = (int) keyLow.getValue(); pp.keyHigh = juce::jmax (pp.keyHigh, pp.keyLow); }); };
    keyHigh.onValueChange   = [this] { edit ([this] (PerfPart& pp) { pp.keyHigh = (int) keyHigh.getValue(); pp.keyLow = juce::jmin (pp.keyLow, pp.keyHigh); }); };
    velLow.onValueChange    = [this] { edit ([this] (PerfPart& pp) { pp.velLow = (int) velLow.getValue(); pp.velHigh = juce::jmax (pp.velHigh, pp.velLow); }); };
    velHigh.onValueChange   = [this] { edit ([this] (PerfPart& pp) { pp.velHigh = (int) velHigh.getValue(); pp.velLow = juce::jmin (pp.velLow, pp.velHigh); }); };
    channel.onValueChange   = [this] { edit ([this] (PerfPart& pp) { pp.midiChannel = (int) channel.getValue(); }); };

    const char* names[] = { "Transp", "Octave", "Low", "High", "Vel lo", "Vel hi", "Channel" };
    juce::Slider* boxes[] = { &transpose, &octave, &keyLow, &keyHigh, &velLow, &velHigh, &channel };
    for (int k = 0; k < 7; ++k)
    {
        styleLabel (labels[k], T (names[k]), 11.5f);
        addAndMakeVisible (labels[k]);
        addAndMakeVisible (boxes[k]);
    }

    tercaButton.setButtonText ("HARM");
    tercaButton.setClickingTogglesState (true);
    tercaButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3ddc84));
    tercaButton.getProperties().set ("fontSize", 12.0f);
    tercaButton.setTooltip (T ("This part gets the harmony (terca) when it is on"));
    tercaButton.onClick = [this] { edit ([this] (PerfPart& pp) { pp.terca = tercaButton.getToggleState(); }); };
    addAndMakeVisible (tercaButton);

    sustainButton.setButtonText ("SUST");
    sustainButton.setClickingTogglesState (true);
    sustainButton.setColour (juce::TextButton::buttonOnColourId, Pal::dim);
    sustainButton.getProperties().set ("fontSize", 12.0f);
    sustainButton.setTooltip (T ("Sustain pedal affects this part"));
    sustainButton.onClick = [this] { edit ([this] (PerfPart& pp) { pp.sustainPedal = sustainButton.getToggleState(); }); };
    addAndMakeVisible (sustainButton);

    exprButton.setButtonText ("EXPR");
    exprButton.setClickingTogglesState (true);
    exprButton.setColour (juce::TextButton::buttonOnColourId, Pal::dim);
    exprButton.getProperties().set ("fontSize", 12.0f);
    exprButton.setTooltip (T ("Expression (CC11) affects this part"));
    exprButton.onClick = [this] { edit ([this] (PerfPart& pp) { pp.expression = exprButton.getToggleState(); }); };
    addAndMakeVisible (exprButton);

    // MIDI Learn (desni klik)
    {
        auto& ml = MidiLearn::get();
        const juce::String id = "perf.part" + juce::String (i + 1) + ".";
        ml.bindButton (onButton, id + "on");
        ml.bindSlider (volume.slider, id + "volume");
        ml.bindSlider (pan.slider, id + "pan");
        ml.bindSlider (reverb.slider, id + "reverb");
        ml.bindSlider (delay.slider, id + "delay");
        ml.bindSlider (transpose, id + "transpose");
        ml.bindSlider (octave, id + "octave");
        ml.bindButton (tercaButton, id + "terca");
        ml.bindButton (sustainButton, id + "sustain");
        ml.bindButton (exprButton, id + "expression");
        // EQ dela (kontrole su u malom prozoru, pa se uci po vrednosti)
        const char* bands[] = { "eq.low", "eq.mid", "eq.high" };
        for (int k = 0; k < 3; ++k)
            ml.addKnob (id + bands[k], -15.0, 15.0, 0.0, [this, k] (double v)
            {
                edit ([k, v] (PerfPart& pp) { (k == 0 ? pp.eqLowDb : k == 1 ? pp.eqMidDb : pp.eqHighDb) = (float) v; });
            });
    }

    refresh();
}

namespace
{
    // Mali prozor sa EQ-om dela: bas, sredina, visoki (dodaje se na EQ usera)
    struct PartEqPanel : public juce::Component
    {
        PartEqPanel (LiveSamplerProcessor& p, int idx, juce::Colour c) : proc (p), index (idx)
        {
            auto db = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
            const auto& part = proc.getPerformance().parts[(size_t) index];
            const int u = proc.findUser (part.user);
            if (u >= 0)
            {
                const auto& us = proc.getUser (u);
                info.setText (T ("User EQ: low ") + db (us.eqLowDb) + T (", mid ") + db (us.eqMidDb)
                              + " @ " + juce::String (juce::roundToInt (us.eqMidFreq)) + T (" Hz, high ") + db (us.eqHighDb)
                              + T ("\nHere you adjust it for this performance only."), juce::dontSendNotification);
            }
            info.setFont (juce::FontOptions (12.0f));
            info.setColour (juce::Label::textColourId, Pal::dim);
            addAndMakeVisible (info);

            const char* names[] = { "Low", "Mid", "High" };
            const float vals[] = { part.eqLowDb, part.eqMidDb, part.eqHighDb };
            for (int k = 0; k < 3; ++k)
            {
                auto* kn = knobs.add (new ValueKnob (T (names[k]), c, -15.0, 15.0, 0.0, db, 0.0));
                kn->setValue (vals[k]);
                const char* bands[] = { "eq.low", "eq.mid", "eq.high" };
                kn->slider.setLearnId ("perf.part" + juce::String (index + 1) + "." + bands[k]);
                kn->onChange = [this, k] (double v)
                {
                    auto perf = proc.getPerformance();
                    auto& pp = perf.parts[(size_t) index];
                    (k == 0 ? pp.eqLowDb : k == 1 ? pp.eqMidDb : pp.eqHighDb) = (float) v;
                    proc.updatePerformance (perf);
                };
                addAndMakeVisible (kn);
            }
            setSize (300, 150);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (6);
            info.setBounds (r.removeFromTop (36));
            const int w = r.getWidth() / 3;
            for (auto* k : knobs) k->setBounds (r.removeFromLeft (w));
        }
        LiveSamplerProcessor& proc;
        const int index;
        juce::Label info;
        juce::OwnedArray<ValueKnob> knobs;
    };
}

namespace Vector
{
    const char* dirName (int d) { static const char* n[] = { "Off", "+X", "-X", "+Y", "-Y" }; return n[juce::jlimit (0, 4, d)]; }

    // ista formula kao u zvuku (SamplerPart): jacina dela za trenutni polozaj joysticka
    float gain (const Performance& perf, const PerfPart& p, float x, float y)
    {
        if (! perf.vectorOn || p.vectorDir == 0) return 1.0f;
        const float a = p.vectorDir == 1 ? x : p.vectorDir == 2 ? -x : p.vectorDir == 3 ? y : -y;
        const float c = p.vectorCenter;
        float v = juce::jlimit (0.0f, 1.0f, a >= 0.0f ? c + (1.0f - c) * a : c * (1.0f + a));
        if (perf.vectorEqualPower) v = std::sin (v * juce::MathConstants<float>::halfPi);
        return v;
    }

    // XY polje: prevuci misem / prstom; prikazuje polozaj koji stize preko CC-a
    struct Pad : public juce::Component, private juce::Timer
    {
        Pad (LiveSamplerProcessor& p) : proc (p) { startTimerHz (30); }
        std::function<void()> onMove, onTick;
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            const bool on = proc.getPerformance().vectorOn;
            g.setColour (Pal::bg);
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (on ? Pal::accent.withAlpha (0.6f) : Pal::dim.withAlpha (0.4f));
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
            g.setColour (Pal::panelHi);
            g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());
            g.drawVerticalLine ((int) r.getCentreX(), r.getY(), r.getBottom());
            const float px = r.getCentreX() + proc.getVectorX() * (r.getWidth() * 0.5f - 4.0f);
            const float py = r.getCentreY() - proc.getVectorY() * (r.getHeight() * 0.5f - 4.0f);
            g.setColour (on ? Pal::lcdText : Pal::dim);
            g.fillEllipse (px - 4.0f, py - 4.0f, 8.0f, 8.0f);
        }
        void mouseDown (const juce::MouseEvent& e) override { move (e); }
        void mouseDrag (const juce::MouseEvent& e) override { move (e); }
        void mouseDoubleClick (const juce::MouseEvent&) override { proc.setVectorXY (0.0f, 0.0f); changed(); }
        void move (const juce::MouseEvent& e)
        {
            auto r = getLocalBounds().toFloat().reduced (5.0f);
            proc.setVectorXY ((e.position.x - r.getCentreX()) / (r.getWidth() * 0.5f), (r.getCentreY() - e.position.y) / (r.getHeight() * 0.5f));
            changed();
        }
        void changed() { repaint(); if (onMove) onMove(); }
        void timerCallback() override { if (proc.takeVectorUiDirty()) changed(); if (onTick) onTick(); }
        LiveSamplerProcessor& proc;
    };
}

void PartStrip::showPartMenu()
{
    juce::PopupMenu m;
    m.addItem (1, T ("Copy part"));
    m.addItem (2, T ("Paste part"), Clip::part.has_value());
    m.addSeparator();
    m.addItem (3, T ("Clear part"));
    {
        const auto& pp = proc.getPerformance().parts[(size_t) index];
        if (pp.isMissing())
        {
            m.addSeparator();
            m.addSectionHeader (pp.missingName);
            m.addItem (200, "Resample from Kronos (Pikaso Autosampler)...");
        }
    }
    {
        const auto& pp = proc.getPerformance().parts[(size_t) index];
        juce::PopupMenu vm;
        for (int d = 0; d <= 4; ++d) vm.addItem (100 + d, T ("Direction: ") + Vector::dirName (d), true, pp.vectorDir == d);
        vm.addSeparator();
        for (int c = 0; c <= 4; ++c) vm.addItem (110 + c, T ("Level at center: ") + juce::String (c * 25) + "%", pp.vectorDir != 0,
                                                 std::abs (pp.vectorCenter - c * 0.25f) < 0.01f);
        m.addSeparator();
        m.addSubMenu (T ("Vector joystick (") + Vector::dirName (pp.vectorDir) + ")", vm);
        m.addItem (300, T ("Follows the oriental tuning (TUNING)"), true, pp.scaleTune);
        m.addSeparator();
        m.addItem (301, T ("Set key range by playing (lowest, then highest key)"));
        m.addItem (302, T ("SPLIT: this part from the key you press upward, others below"));
    }
    juce::Component::SafePointer<PartStrip> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&partMenu), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 300) { safe->edit ([] (PerfPart& pp) { pp.scaleTune = ! pp.scaleTune; }); return; }
        if (r == 301 || r == 302) { safe->proc.armKeyLearn (safe->index, r == 301 ? 1 : 2); return; }
        if (r == 200)
        {
            const auto& pp = safe->proc.getPerformance().parts[(size_t) safe->index];
            KronosPage::startAutosampler ("--pcg \"" + pp.missingPcg + "\" --program " + pp.missingSource);
            return;
        }
        if (r >= 100 && r <= 104) { safe->edit ([r] (PerfPart& pp) { pp.vectorDir = r - 100; }); return; }
        if (r >= 110 && r <= 114) { safe->edit ([r] (PerfPart& pp) { pp.vectorCenter = (float) (r - 110) * 0.25f; }); return; }
        if (r == 1) { Clip::part = safe->proc.getPerformance().parts[(size_t) safe->index]; return; }
        safe->edit ([r] (PerfPart& pp) { pp = (r == 2 && Clip::part) ? *Clip::part : PerfPart(); });
    });
}

void PartStrip::showEq()
{
    auto* top = getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::make_unique<PartEqPanel> (proc, index, colour),
                                            top->getLocalArea (this, eqButton.getBounds()), top);
}

void PartStrip::edit (std::function<void (PerfPart&)> fn)
{
    auto perf = proc.getPerformance();
    fn (perf.parts[(size_t) index]);
    proc.updatePerformance (perf);
}

void PartStrip::stepUser (int dir)
{
    const int n = proc.getNumUsers();
    if (n == 0) return;
    const int cur = proc.findUser (proc.getPerformance().parts[(size_t) index].user);
    const int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : juce::jlimit (0, n - 1, cur + dir);
    const auto name = proc.getUser (next).name;
    edit ([name] (PerfPart& pp) { pp.user = name; pp.on = true; });
}

void PartStrip::showUserMenu()
{
    const auto& part = proc.getPerformance().parts[(size_t) index];
    juce::StringArray labels, values;
    for (int i = 0; i < proc.getNumUsers(); ++i)
    {
        const auto& u = proc.getUser (i);
        labels.add (juce::String (i + 1).paddedLeft ('0', 3) + "  " + u.name + (u.category.isNotEmpty() ? "   [" + u.category + "]" : juce::String()));
        values.add (u.name);
    }
    juce::Component::SafePointer<PartStrip> safe (this);
    Picker::show (userButton, labels, values, { "+ New user from instrument...", "+ Empty new user", "(no user)" }, part.user,
        [safe] (int action, const juce::String& name)
        {
            if (safe == nullptr) return;
            auto& self = *safe;
            if (action == 0)
            {
                UI::showInstrumentMenu (&self.userButton, self.proc.getInstrumentList(), {}, false,
                    [safe] (const juce::String& inst)
                    {
                        if (safe == nullptr || inst.isEmpty()) return;
                        const int u = safe->proc.createUserFromInstrument (inst);
                        const auto nm = safe->proc.getUser (u).name;
                        safe->edit ([nm] (PerfPart& pp) { pp.user = nm; pp.on = true; });
                    });
            }
            else if (action == 1)
            {
                const int u = self.proc.createUser ("User");
                const auto nm = self.proc.getUser (u).name;
                self.edit ([nm] (PerfPart& pp) { pp.user = nm; pp.on = true; });
                self.editor.editUser (nm);
            }
            else if (action == 2)
                self.edit ([] (PerfPart& pp) { pp.user = {}; pp.on = false; });
            else if (name.isNotEmpty())
                self.edit ([name] (PerfPart& pp) { pp.user = name; pp.on = true; });
        });
}

void PartStrip::refresh()
{
    const auto& p = proc.getPerformance().parts[(size_t) index];
    onButton.setToggleState (p.on, juce::dontSendNotification);
    userButton.setButtonText (p.isMissing() ? juce::String::fromUTF8 ("\xe2\x9a\xa0 ") + p.missingName : p.user.isEmpty() ? T ("— choose user —") : p.user);
    userButton.setColour (juce::TextButton::buttonColourId, p.isMissing() ? juce::Colour (0xff5a2230) : Pal::bg);
    userButton.setTooltip (p.isMissing() ? "This Kronos program is not resampled yet, so the part is silent. Right side menu: Resample from Kronos, "
                                           "or choose another user. When it is resampled, it appears here by itself (after Refresh)."
                                         : juce::String ("Choose the user (type to search)"));
    editButton.setEnabled (p.user.isNotEmpty());
    volume.setValue (p.volumeDb);
    pan.setValue (p.pan);
    reverb.setValue (p.reverbSend);
    delay.setValue (p.delaySend);
    setSliderValue (transpose, p.transpose);
    setSliderValue (octave, p.octave);
    setSliderValue (keyLow, p.keyLow);
    setSliderValue (keyHigh, p.keyHigh);
    setSliderValue (velLow, p.velLow);
    setSliderValue (velHigh, p.velHigh);
    setSliderValue (channel, p.midiChannel);
    keyLow.updateText();
    keyHigh.updateText();
    tercaButton.setToggleState (p.terca, juce::dontSendNotification);
    sustainButton.setToggleState (p.sustainPedal, juce::dontSendNotification);
    exprButton.setToggleState (p.expression, juce::dontSendNotification);
    setAlpha (p.on ? 1.0f : 0.55f);
}

void PartStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r, 8.0f);
    const auto& perf = proc.getPerformance();
    const auto& p = perf.parts[(size_t) index];
    // vector: traka u boji dela pokazuje trenutnu jacinu od joysticka
    const float vg = Vector::gain (perf, p, proc.getVectorX(), proc.getVectorY());
    auto bar = r.removeFromTop (4.0f).reduced (8.0f, 0.0f);
    g.setColour (colour.withAlpha (0.25f));
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (colour);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * vg), 2.0f);
}

void PartStrip::paintOverChildren (juce::Graphics& g)
{
    const auto& perf = proc.getPerformance();
    const auto& p = perf.parts[(size_t) index];
    if (! perf.vectorOn || p.vectorDir == 0) return;
    const float vg = Vector::gain (perf, p, proc.getVectorX(), proc.getVectorY());
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.setColour (Pal::lcdText);
    g.drawText ("VJS " + juce::String (Vector::dirName (p.vectorDir)) + " " + juce::String ((int) std::round (vg * 100.0f)) + "%",
                getLocalBounds().removeFromTop (14).reduced (10, 0), juce::Justification::centredRight);
    if (vg < 0.05f)
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));     // utisan joystickom
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    }
}

void PartStrip::resized()
{
    auto r = getLocalBounds().reduced (6);
    r.removeFromTop (4);
    {
        auto row = r.removeFromTop (26);
        partMenu.setBounds (row.removeFromRight (20));
        row.removeFromRight (2);
        onButton.setBounds (row);
    }
    r.removeFromTop (4);
    {
        auto row = r.removeFromTop (40);
        userPrev.setBounds (row.removeFromLeft (19).reduced (0, 4));
        userNext.setBounds (row.removeFromRight (19).reduced (0, 4));
        userButton.setBounds (row.reduced (1, 0));
    }
    r.removeFromTop (2);
    {
        auto row = r.removeFromTop (20);
        eqButton.setBounds (row.removeFromRight (40));
        row.removeFromRight (3);
        editButton.setBounds (row);
    }
    r.removeFromTop (4);

    const bool compact = getWidth() < 120;   // svih 16 delova na ekranu
    if (compact)
    {
        for (auto* k : { &volume, &pan, &reverb, &delay })
            k->setBounds (r.removeFromTop (46));
    }
    else
    {
        // slobodan prostor (bez KEYS redova) ide na vece dugmice
        const int need = 120 + 4 + (proc.getShowKeyRows() ? 7 : 2) * 25 + 4 + 24;
        const int kh = 60 + juce::jlimit (0, 30, (r.getHeight() - need) / 2);
        auto knobs1 = r.removeFromTop (kh);
        volume.setBounds (knobs1.removeFromLeft (knobs1.getWidth() / 2));
        pan.setBounds (knobs1);
        auto knobs2 = r.removeFromTop (kh);
        reverb.setBounds (knobs2.removeFromLeft (knobs2.getWidth() / 2));
        delay.setBounds (knobs2);
    }
    r.removeFromTop (4);

    juce::Slider* boxes[] = { &transpose, &octave, &keyLow, &keyHigh, &velLow, &velHigh, &channel };
    const bool keyRows = proc.getShowKeyRows();
    for (int k = 0; k < 7; ++k)
    {
        const bool vis = k < 2 || keyRows;   // Transp / Octave uvek; tasteri, jacine i kanal na dugme KEYS (ili prevlacenje dole)
        labels[k].setVisible (vis);
        boxes[k]->setVisible (vis);
        if (! vis) continue;
        auto row = r.removeFromTop (compact ? 22 : 25);
        labels[k].setBounds (row.removeFromLeft (compact ? 34 : 46));
        boxes[k]->setBounds (row.reduced (0, 1));
        boxes[k]->setTextBoxStyle (juce::Slider::TextBoxLeft, false, juce::jmax (40, row.getWidth() - 40), 22);
    }
    r.removeFromTop (4);
    auto toggles = r.removeFromTop (24);
    const int tw = toggles.getWidth() / 3;
    tercaButton.setBounds (toggles.removeFromLeft (tw).reduced (1, 0));
    sustainButton.setBounds (toggles.removeFromLeft (tw).reduced (1, 0));
    exprButton.setBounds (toggles.reduced (1, 0));
}

//==============================================================================
//  KeyRangeView
//==============================================================================
static constexpr int kRangeLo = 21, kRangeHi = 108;

juce::Rectangle<float> KeyRangeView::barArea() const
{
    auto area = getLocalBounds().toFloat().reduced (10.0f, 6.0f);
    area.removeFromBottom (19.0f);
    return area;
}

juce::Array<int> KeyRangeView::activeParts() const
{
    const auto& perf = proc.getPerformance();
    juce::Array<int> active;
    for (int i = 0; i < kNumParts; ++i)
        if (perf.parts[(size_t) i].on && (perf.parts[(size_t) i].user.isNotEmpty() || perf.parts[(size_t) i].isMissing()))
            active.add (i);
    return active;
}

int KeyRangeView::keyAt (float x) const
{
    const auto area = barArea();
    const float kw = area.getWidth() / (float) (kRangeHi - kRangeLo + 1);
    return juce::jlimit (0, 127, kRangeLo + (int) std::floor ((x - area.getX()) / kw));
}

KeyRangeView::Hit KeyRangeView::hitTest (juce::Point<float> pt) const
{
    const auto area = barArea();
    const auto active = activeParts();
    if (active.isEmpty()) return {};
    const float kw = area.getWidth() / (float) (kRangeHi - kRangeLo + 1);
    const float rowH = juce::jmin (16.0f, area.getHeight() / (float) active.size());
    const int k = (int) std::floor ((pt.y - area.getY()) / rowH);
    if (k < 0 || k >= active.size()) return {};
    const auto& p = proc.getPerformance().parts[(size_t) active[k]];
    const float x0 = area.getX() + (float) (juce::jlimit (kRangeLo, kRangeHi, p.keyLow) - kRangeLo) * kw;
    const float x1 = area.getX() + (float) (juce::jlimit (kRangeLo, kRangeHi, p.keyHigh) - kRangeLo + 1) * kw;
    const float grab = juce::jmax (8.0f, kw * 1.5f);   // prst je sirok, ivica se hvata i malo van zone
    if (pt.x < x0 - grab || pt.x > x1 + grab) return {};
    Hit h; h.part = active[k];
    if (std::abs (pt.x - x0) <= grab && pt.x < (x0 + x1) * 0.5f) h.mode = 1;
    else if (std::abs (pt.x - x1) <= grab) h.mode = 2;
    else h.mode = 3;
    return h;
}

void KeyRangeView::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    setMouseCursor (h.mode == 1 || h.mode == 2 ? juce::MouseCursor::LeftRightResizeCursor
                    : h.mode == 3 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void KeyRangeView::mouseDown (const juce::MouseEvent& e)
{
    drag = hitTest (e.position);
    if (drag.part < 0) return;
    const auto& p = proc.getPerformance().parts[(size_t) drag.part];
    dragStartKey = keyAt (e.position.x);
    origLo = juce::jmin (p.keyLow, p.keyHigh);
    origHi = juce::jmax (p.keyLow, p.keyHigh);
    repaint();
}

void KeyRangeView::mouseDrag (const juce::MouseEvent& e)
{
    if (drag.part < 0) return;
    const int key = keyAt (e.position.x);
    int lo = origLo, hi = origHi;
    if (drag.mode == 1) lo = juce::jmin (key, hi);
    else if (drag.mode == 2) hi = juce::jmax (key, lo);
    else
    {
        // pomeranje cele zone; ako je zona do kraja (0 / 127) ta ivica ostaje na kraju
        int d = key - dragStartKey;
        d = juce::jlimit (-lo, 127 - hi, d);
        lo += d; hi += d;
    }
    auto perf = proc.getPerformance();
    auto& pp = perf.parts[(size_t) drag.part];
    if (pp.keyLow == lo && pp.keyHigh == hi) return;
    pp.keyLow = lo; pp.keyHigh = hi;
    proc.updatePerformance (perf);
    repaint();
}

void KeyRangeView::mouseUp (const juce::MouseEvent&)
{
    drag = {};
    repaint();
}

void KeyRangeView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r, 8.0f);

    auto area = r.reduced (10.0f, 6.0f);
    auto keys = area.removeFromBottom (16.0f);
    area.removeFromBottom (3.0f);
    const int lo = kRangeLo, hi = kRangeHi;
    const float kw = area.getWidth() / (float) (hi - lo + 1);
    auto xOf = [&] (int n) { return area.getX() + (float) (n - lo) * kw; };

    // mala klavijatura
    g.setColour (juce::Colour (0xffdcdcdc));
    g.fillRoundedRectangle (keys, 2.0f);
    for (int n = lo; n <= hi; ++n)
    {
        const int pc = n % 12;
        if (pc == 0 || pc == 5)
        {
            g.setColour (juce::Colour (0xff9a9a9a));
            g.drawVerticalLine (juce::roundToInt (xOf (n)), keys.getY(), keys.getBottom());
        }
        if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
        {
            g.setColour (juce::Colour (0xff1a1a1a));
            g.fillRect (xOf (n), keys.getY(), kw, keys.getHeight() * 0.62f);
        }
    }

    const auto& perf = proc.getPerformance();
    const auto active = activeParts();

    if (active.isEmpty())
    {
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (T ("No part is on"), area, juce::Justification::centred);
        return;
    }

    const float rowH = juce::jmin (16.0f, area.getHeight() / (float) active.size());
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    for (int k = 0; k < active.size(); ++k)
    {
        const int i = active[k];
        const auto& p = perf.parts[(size_t) i];
        const int a = juce::jlimit (lo, hi, p.keyLow), b = juce::jlimit (lo, hi, p.keyHigh);
        if (b < a) continue;
        auto bar = juce::Rectangle<float> (xOf (a), area.getY() + (float) k * rowH + 1.0f, (float) (b - a + 1) * kw, rowH - 2.0f);
        g.setColour (Pal::part[i].withAlpha (drag.part == i ? 1.0f : 0.85f));
        g.fillRoundedRectangle (bar, 3.0f);
        if (drag.part == i)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (bar, 3.0f, 1.5f);
            // granice zone na klavijaturi dok se vuce
            g.setColour (Pal::part[i].withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (xOf (a), keys.getY(), (float) (b - a + 1) * kw, keys.getHeight()));
        }
        g.setColour (juce::Colours::black);
        juce::String txt = juce::String (i + 1) + "  " + (p.isMissing() ? juce::String::fromUTF8 ("\xe2\x9a\xa0 ") + p.missingName : p.user);
        if (p.transpose != 0 || p.octave != 0)
            txt << "  (" << (p.octave * 12 + p.transpose > 0 ? "+" : "") << (p.octave * 12 + p.transpose) << ")";
        if (drag.part == i)
            txt << "   " << juce::MidiMessage::getMidiNoteName (p.keyLow, true, true, 4) << " - " << juce::MidiMessage::getMidiNoteName (p.keyHigh, true, true, 4);
        g.drawText (txt, bar.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
    }
}

//==============================================================================
//  TuningBar
//==============================================================================
TuningBar::TuningBar (LiveSamplerProcessor& p) : proc (p)
{
    title.setText ("TUNING", juce::dontSendNotification);
    title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (title);
    onButton.setButtonText ("ON");
    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    onButton.setTooltip (T ("Oriental tuning (quarter tones) of this performance on / off"));
    onButton.onClick = [this] { proc.setTuning (onButton.getToggleState()); };
    MidiLearn::get().bindButton (onButton, "tuning.on");
    addAndMakeVisible (onButton);
    qtButton.setButtonText ("QT EDIT");
    qtButton.setClickingTogglesState (true);
    qtButton.setColour (juce::TextButton::buttonOnColourId, Pal::red);
    qtButton.setTooltip (T ("While on, the keys you press switch that note between 0 and -50 cents (quarter tone) instead of playing. Turn off to play."));
    qtButton.onClick = [this] { proc.setQtEdit (qtButton.getToggleState()); };
    MidiLearn::get().bindButton (qtButton, "tuning.qtedit");
    addAndMakeVisible (qtButton);
    presetButton.setTooltip (T ("Choose a maqam (scale) and its tonic"));
    presetButton.onClick = [this]
    {
        juce::PopupMenu m;
        const auto& ps = Tuning::presets();
        for (int i = 0; i < (int) ps.size(); ++i)
        {
            if (i == 0) { m.addItem (1000, ps[0].name); continue; }
            juce::PopupMenu roots;
            for (int r = 0; r < 12; ++r) roots.addItem (1000 + i * 12 + r, juce::String (ps[(size_t) i].name) + " " + Terca::pcName (r));
            m.addSubMenu (ps[(size_t) i].name, roots);
        }
        juce::Component::SafePointer<TuningBar> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetButton), [safe] (int r)
        {
            if (safe == nullptr || r < 1000) return;
            const int v = r - 1000;
            if (v == 0) safe->proc.setTuningPreset (0, 0);
            else safe->proc.setTuningPreset (v / 12, v % 12);
        });
    };
    addAndMakeVisible (presetButton);
    for (int pc = 0; pc < 12; ++pc)
    {
        cells[pc].onClick = [this, pc]
        {
            juce::PopupMenu m;
            const float vals[] = { -50, -40, -30, -20, -14, -10, 0, 10, 14, 20, 30, 50 };
            const float cur = proc.getPerformance().tuneCents[(size_t) pc];
            for (int k = 0; k < 12; ++k)
                m.addItem (k + 1, (vals[k] > 0 ? "+" : "") + juce::String ((int) vals[k]) + " cents", true, std::abs (cur - vals[k]) < 0.5f);
            juce::Component::SafePointer<TuningBar> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&cells[pc]), [safe, pc, vals] (int r)
            {
                if (safe != nullptr && r > 0) safe->proc.setTuneCents (pc, vals[r - 1]);
            });
        };
        addAndMakeVisible (cells[pc]);
    }
}

void TuningBar::refresh()
{
    const auto& p = proc.getPerformance();
    onButton.setToggleState (p.tuneOn, juce::dontSendNotification);
    qtButton.setToggleState (proc.isQtEdit(), juce::dontSendNotification);
    presetButton.setButtonText (p.tuneName.isNotEmpty() ? p.tuneName : T ("Maqam..."));
    for (int pc = 0; pc < 12; ++pc)
    {
        const float c = p.tuneCents[(size_t) pc];
        const bool moved = std::abs (c) > 0.5f;
        cells[pc].setButtonText (Terca::pcName (pc) + (moved ? "\n" + juce::String ((c > 0 ? "+" : "") + juce::String ((int) std::lround (c))) : juce::String()));
        cells[pc].setColour (juce::TextButton::buttonColourId, moved ? juce::Colour (0xffffb340).withAlpha (p.tuneOn ? 0.85f : 0.35f) : Pal::panelHi);
        cells[pc].setColour (juce::TextButton::textColourOffId, moved && p.tuneOn ? juce::Colours::black : Pal::text);
    }
}

void TuningBar::resized()
{
    auto r = getLocalBounds();
    title.setBounds (r.removeFromLeft (86));
    onButton.setBounds (r.removeFromLeft (64).reduced (2, 0));
    presetButton.setBounds (r.removeFromLeft (170).reduced (2, 0));
    qtButton.setBounds (r.removeFromRight (100).reduced (2, 0));
    r.removeFromLeft (6);
    const int w = r.getWidth() / 12;
    for (auto& c : cells) c.setBounds (r.removeFromLeft (w).reduced (2, 0));
}

//==============================================================================
//  LivePage
//==============================================================================
LivePage::LivePage (LiveSamplerProcessor& p, LiveSamplerEditor& e) : proc (p), editor (e)
{
    gigButton.setButtonText ("GIG MODE");
    gigButton.setClickingTogglesState (true);
    gigButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3ddc84));
    gigButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    gigButton.setTooltip (T ("For the stage: the screen does not sleep, the program runs at high priority, less drawing"));
    gigButton.onClick = [this] { proc.setGigMode (gigButton.getToggleState()); };
    addAndMakeVisible (gigButton);
    cpuLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    cpuLabel.setJustificationType (juce::Justification::centred);
    cpuLabel.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (cpuLabel);
    addAndMakeVisible (scenes);
    addAndMakeVisible (tuning);
    auto big = [this] (LearnButton& b, const juce::String& t, float fs)
    {
        b.setButtonText (t);
        b.getProperties().set ("fontSize", fs);
        addAndMakeVisible (b);
    };
    big (prevButton, juce::String::fromUTF8 ("\xe2\x97\x80 PERF"), 18.0f);
    big (nextButton, juce::String::fromUTF8 ("PERF \xe2\x96\xb6"), 18.0f);
    prevButton.onClick = [this] { proc.stepPerformance (-1); };
    nextButton.onClick = [this] { proc.stepPerformance (1); };
    MidiLearn::get().bindButton (prevButton, "live.perfprev");
    MidiLearn::get().bindButton (nextButton, "live.perfnext");
    big (transDown, juce::String::fromUTF8 ("\xe2\x88\x92"), 26.0f);
    big (transUp, "+", 26.0f);
    big (transZero, "0", 16.0f);
    transDown.onClick = [this] { proc.setGlobalTranspose (proc.getGlobalTranspose() - 1); };
    transUp.onClick = [this] { proc.setGlobalTranspose (proc.getGlobalTranspose() + 1); };
    transZero.onClick = [this] { proc.setGlobalTranspose (0); };
    transZero.setTooltip (T ("Transpose back to 0"));
    MidiLearn::get().bindButton (transDown, "live.transdown");
    MidiLearn::get().bindButton (transUp, "live.transup");
    MidiLearn::get().bindButton (transZero, "live.transzero");
    big (harmButton, "HARMONY", 18.0f);
    harmButton.onClick = [this] { proc.setTercaOn (! proc.isTercaOn()); };
    MidiLearn::get().bindButton (harmButton, "live.harmony");
    big (panicButton, "PANIC", 18.0f);
    panicButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7a2230));
    panicButton.onClick = [this] { proc.panic(); };
    big (lockButton, "LOCK", 18.0f);
    lockButton.setTooltip (T ("Locks editing on stage: only LIVE, SET LIST, scenes, part volumes and performance change work"));
    lockButton.onClick = [this] { proc.setLiveLock (! proc.isLiveLocked()); };

    for (int i = 0; i < kNumParts; ++i)
    {
        auto& f = faders[i];
        f.volume.setSliderStyle (juce::Slider::LinearBarVertical);
        f.volume.setRange (-60.0, 6.0, 0.1);
        f.volume.setSkewFactorFromMidPoint (-12.0);
        f.volume.setDoubleClickReturnValue (true, 0.0);
        f.volume.setTextValueSuffix (" dB");
        f.volume.setNumDecimalPlacesToDisplay (1);
        f.volume.setColour (juce::Slider::trackColourId, Pal::part[i].withAlpha (0.75f));
        f.volume.setColour (juce::Slider::textBoxTextColourId, Pal::text);
        f.volume.onValueChange = [this, i]
        {
            auto perf = proc.getPerformance();
            perf.parts[(size_t) i].volumeDb = (float) faders[i].volume.getValue();
            proc.updatePerformance (perf);
        };
        MidiLearn::get().bindSlider (f.volume, "perf.part" + juce::String (i + 1) + ".volume");
        f.on.setClickingTogglesState (true);
        f.on.setColour (juce::TextButton::buttonOnColourId, Pal::part[i]);
        f.on.getProperties().set ("fontSize", 14.0f);
        f.on.onClick = [this, i]
        {
            auto perf = proc.getPerformance();
            perf.parts[(size_t) i].on = faders[i].on.getToggleState();
            proc.updatePerformance (perf);
        };
        MidiLearn::get().bindButton (f.on, "perf.part" + juce::String (i + 1) + ".on");
        f.name.setJustificationType (juce::Justification::centred);
        f.name.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        f.name.setMinimumHorizontalScale (0.6f);
        for (juce::Component* c : { (juce::Component*) &f.volume, (juce::Component*) &f.on, (juce::Component*) &f.name })
            addAndMakeVisible (c);
    }
}

void LivePage::refresh()
{
    gigButton.setToggleState (proc.isGigMode(), juce::dontSendNotification);
    updateMeter();
    scenes.refresh();
    tuning.refresh();
    const auto& perf = proc.getPerformance();
    for (int i = 0; i < kNumParts; ++i)
    {
        const auto& pp = perf.parts[(size_t) i];
        auto& f = faders[i];
        const bool has = pp.user.isNotEmpty() || pp.isMissing();
        f.volume.setValue (pp.volumeDb, juce::dontSendNotification);
        f.on.setToggleState (pp.on, juce::dontSendNotification);
        f.on.setButtonText (juce::String (i + 1));
        f.name.setText (pp.isMissing() ? juce::String::fromUTF8 ("\xe2\x9a\xa0 ") + pp.missingName : pp.user, juce::dontSendNotification);
        f.name.setColour (juce::Label::textColourId, pp.on ? Pal::text : Pal::dim);
        for (juce::Component* c : { (juce::Component*) &f.volume, (juce::Component*) &f.on, (juce::Component*) &f.name })
            c->setAlpha (has ? 1.0f : 0.25f);
        f.volume.setEnabled (has);
        f.on.setEnabled (has);
    }
    harmButton.setToggleState (proc.isTercaOn(), juce::dontSendNotification);
    harmButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3ddc84));
    harmButton.setButtonText ("HARMONY\n" + proc.getTercaText());
    lockButton.setToggleState (proc.isLiveLocked(), juce::dontSendNotification);
    lockButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    lockButton.setButtonText (proc.isLiveLocked() ? juce::String::fromUTF8 ("LOCKED") : juce::String ("LOCK"));
    repaint();
}

void LivePage::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    // veliki displej: performans, mesto, scena
    g.setColour (Pal::lcdBg);
    g.fillRoundedRectangle (lcd.toFloat(), 10.0f);
    const auto& perf = proc.getPerformance();
    const int slot = proc.getCurrentSlot();
    auto r = lcd.reduced (18, 8);
    g.setColour (Pal::lcdText.withAlpha (0.7f));
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    juce::String top = (slot >= 0 ? proc.getBankName (slot / kPerfPerBank) + "   " + juce::String (slot % kPerfPerBank).paddedLeft ('0', 3) : juce::String ("---"));
    if (perf.scene >= 0 && perf.scenes[(size_t) perf.scene].used)
        top << "   |   SCENE " << (perf.scene + 1) << ": " << perf.scenes[(size_t) perf.scene].name;
    if (proc.isLiveLocked()) top << "   |   LOCKED";
    g.drawText (top, r.removeFromTop (22), juce::Justification::centredLeft, true);
    g.setColour (Pal::lcdText);
    g.setFont (juce::FontOptions ((float) juce::jmin (60, r.getHeight()), juce::Font::bold));
    g.drawFittedText (perf.name + (proc.isPerformanceDirty() ? " *" : ""), r, juce::Justification::centredLeft, 1, 0.6f);
    // transpose
    g.setColour (Pal::lcdBg);
    g.fillRoundedRectangle (transArea.toFloat(), 8.0f);
    const int t = proc.getGlobalTranspose();
    g.setColour (t != 0 ? juce::Colour (0xffffb340) : Pal::lcdText);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("TRANSPOSE", transArea.withHeight (20).translated (0, 4), juce::Justification::centred);
    g.setFont (juce::FontOptions (34.0f, juce::Font::bold));
    g.drawText ((t > 0 ? "+" : "") + juce::String (t), transArea.withTrimmedTop (18), juce::Justification::centred);
}

void LivePage::updateMeter()
{
    const auto a = proc.getAudioInfo();
    juce::String t = "CPU " + juce::String (juce::roundToInt (proc.getAudioLoad() * 100.0f)) + "%";
    if (a.latencyMs > 0) t << "  " << juce::String (a.latencyMs, 1) << " ms";
    t << "\n" << (a.driver.isNotEmpty() ? a.driver : juce::String ("-")) << "  " << a.buffer << " smp";
    cpuLabel.setText (t, juce::dontSendNotification);
    cpuLabel.setColour (juce::Label::textColourId, a.warn ? juce::Colour (0xffffb340) : Pal::dim);
    cpuLabel.setTooltip (a.advice.isNotEmpty() ? a.advice : T ("Audio OK"));
}

void LivePage::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (110);
    auto right = top.removeFromRight (430);
    lcd = top.reduced (0, 0);
    // desno: prethodni / sledeci performans, transpose
    auto pr = right.removeFromLeft (150).reduced (6, 0);
    prevButton.setBounds (pr.removeFromTop (52).reduced (0, 2));
    nextButton.setBounds (pr.reduced (0, 2));
    transArea = right.removeFromLeft (130).reduced (4, 0);
    auto tb = right.reduced (4, 0);
    transUp.setBounds (tb.removeFromTop (36).reduced (0, 2));
    transZero.setBounds (tb.removeFromTop (34).reduced (0, 2));
    transDown.setBounds (tb.reduced (0, 2));
    r.removeFromTop (10);
    scenes.setBounds (r.removeFromTop (76));
    r.removeFromTop (8);
    tuning.setBounds (r.removeFromTop (44));
    r.removeFromTop (8);
    auto side = r.removeFromRight (170);
    const int sh = side.getHeight();
    harmButton.setBounds (side.removeFromTop (juce::jmin (90, sh * 28 / 100)).reduced (4));
    gigButton.setBounds (side.removeFromTop (juce::jmin (54, sh * 17 / 100)).reduced (4));
    cpuLabel.setBounds (side.removeFromTop (36).reduced (4, 0));
    lockButton.setBounds (side.removeFromBottom (juce::jmin (70, sh * 20 / 100)).reduced (4));
    panicButton.setBounds (side.removeFromBottom (juce::jmin (70, sh * 20 / 100)).reduced (4));
    r.removeFromRight (8);
    const int w = r.getWidth() / kNumParts;
    for (int i = 0; i < kNumParts; ++i)
    {
        auto col = r.removeFromLeft (w).reduced (3, 0);
        faders[i].name.setBounds (col.removeFromTop (34));
        faders[i].on.setBounds (col.removeFromBottom (36).reduced (0, 2));
        faders[i].volume.setBounds (col.reduced (4, 2));
    }
}

//==============================================================================
//  SceneBar
//==============================================================================
SceneBar::SceneBar (LiveSamplerProcessor& p, bool b) : proc (p), big (b)
{
    title.setText ("SCENES", juce::dontSendNotification);
    title.setFont (juce::FontOptions (big ? 14.0f : 12.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (title);
    for (int i = 0; i < kNumScenes; ++i)
    {
        scenes[i].setClickingTogglesState (false);
        scenes[i].getProperties().set ("fontSize", big ? 16.0f : 12.0f);
        scenes[i].onClick = [this, i] { clicked (i); };
        addAndMakeVisible (scenes[i]);
        MidiLearn::get().bindButton (scenes[i], "scene." + juce::String (i + 1));
    }
    storeButton.setButtonText ("STORE");
    storeButton.setClickingTogglesState (true);
    storeButton.setColour (juce::TextButton::buttonOnColourId, Pal::red);
    storeButton.setTooltip (T ("Turn on, then click a scene: it remembers which parts are on, their volume, pan, transpose, octave, harmony, keys and sends."));
    addAndMakeVisible (storeButton);
    moreButton.setButtonText (juce::String::fromUTF8 ("\xe2\x8b\xaf"));
    moreButton.setTooltip (T ("Rename / clear a scene"));
    moreButton.onClick = [this] { menu(); };
    addAndMakeVisible (moreButton);
}

void SceneBar::clicked (int s)
{
    if (storeButton.getToggleState())
    {
        proc.storeScene (s);
        storeButton.setToggleState (false, juce::dontSendNotification);
    }
    else
        proc.recallScene (s);
    refresh();
}

void SceneBar::menu()
{
    juce::PopupMenu m, ren, clr;
    const auto& p = proc.getPerformance();
    for (int i = 0; i < kNumScenes; ++i)
        if (p.scenes[(size_t) i].used)
        {
            ren.addItem (100 + i, juce::String (i + 1) + "  " + p.scenes[(size_t) i].name);
            clr.addItem (200 + i, juce::String (i + 1) + "  " + p.scenes[(size_t) i].name);
        }
    m.addSubMenu (T ("Rename scene"), ren, ren.getNumItems() > 0);
    m.addSubMenu (T ("Clear scene"), clr, clr.getNumItems() > 0);
    juce::Component::SafePointer<SceneBar> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r >= 200) { safe->proc.clearScene (r - 200); safe->refresh(); return; }
        const int s = r - 100;
        auto* w = new juce::AlertWindow (T ("Scene name"), {}, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("n", safe->proc.getPerformance().scenes[(size_t) s].name);
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (T ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, s, w] (int ok)
        {
            if (safe == nullptr || ok == 0) return;
            safe->proc.renameScene (s, w->getTextEditorContents ("n"));
            safe->refresh();
        }), true);
    });
}

void SceneBar::refresh()
{
    const auto& p = proc.getPerformance();
    for (int i = 0; i < kNumScenes; ++i)
    {
        const auto& sc = p.scenes[(size_t) i];
        const bool cur = p.scene == i && sc.used;
        scenes[i].setButtonText (sc.used ? (big ? juce::String (i + 1) + "\n" + sc.name : juce::String (i + 1) + " " + sc.name) : juce::String (i + 1));
        scenes[i].setColour (juce::TextButton::buttonColourId, cur ? Pal::accent : sc.used ? juce::Colour (0xff2f5d8a) : Pal::panelHi.darker (0.3f));
        scenes[i].setColour (juce::TextButton::textColourOffId, cur ? juce::Colours::black : sc.used ? Pal::text : Pal::dim);
    }
    moreButton.setEnabled (std::any_of (p.scenes.begin(), p.scenes.end(), [] (const Scene& s) { return s.used; }));
}

void SceneBar::resized()
{
    auto r = getLocalBounds();
    title.setBounds (r.removeFromLeft (big ? 86 : 64));
    moreButton.setBounds (r.removeFromRight (big ? 56 : 34).reduced (2, 0));
    storeButton.setBounds (r.removeFromRight (big ? 110 : 72).reduced (2, 0));
    r.removeFromRight (6);
    const int w = r.getWidth() / kNumScenes;
    for (auto& b : scenes) b.setBounds (r.removeFromLeft (w).reduced (2, 0));
}

//==============================================================================
//  PerformancePage
//==============================================================================
PerformancePage::PerformancePage (LiveSamplerProcessor& p, LiveSamplerEditor& e)
    : proc (p), editor (e), ranges (p)
{
    keysButton.setButtonText (T ("KEYS / VEL"));
    keysButton.setClickingTogglesState (true);
    keysButton.setTooltip (T ("Show Low / High key, velocity and MIDI channel on every part. Key ranges can also be dragged in the bar below, or set by playing (part menu)."));
    keysButton.onClick = [this] { proc.setShowKeyRows (keysButton.getToggleState()); for (auto* s : strips) s->resized(); };
    addAndMakeVisible (keysButton);
    styleLabel (nameLabel, T ("PERFORMANCE NAME"), 12.5f);
    addAndMakeVisible (nameLabel);

    nameEdit.setEditable (true);
    nameEdit.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    nameEdit.setColour (juce::Label::backgroundColourId, Pal::bg);
    nameEdit.setColour (juce::Label::textColourId, Pal::lcdText);
    nameEdit.onTextChange = [this]
    {
        auto perf = proc.getPerformance();
        perf.name = nameEdit.getText().trim().isEmpty() ? juce::String ("Performance") : nameEdit.getText().trim();
        proc.updatePerformance (perf);
    };
    addAndMakeVisible (nameEdit);

    for (int i = 0; i < kNumParts; ++i)
        addChildComponent (strips.add (new PartStrip (p, e, i)));
    addAndMakeVisible (ranges);
    addAndMakeVisible (sceneBar);

    const char* pageNames[] = { "PARTS 1-8", "PARTS 9-16" };
    for (int b = 0; b < 2; ++b)
    {
        pageButtons[b].setButtonText (pageNames[b]);
        pageButtons[b].setClickingTogglesState (false);
        pageButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        pageButtons[b].onClick = [this, b] { page = b; resized(); refresh(); };
        addAndMakeVisible (pageButtons[b]);
    }

    saveButton.setButtonText (T ("SAVE"));
    saveButton.setColour (juce::TextButton::buttonColourId, Pal::red.withAlpha (0.8f));
    saveButton.setTooltip (T ("Opens LISTS ready to write: click the slot where this performance is saved"));
    saveButton.onClick = [this] { editor.showTab (LiveSamplerEditor::TabLists); editor.armListWrite(); };
    addAndMakeVisible (saveButton);

    auto* pad = new Vector::Pad (proc);
    pad->onMove = [this] { for (auto* s : strips) s->repaint(); };
    pad->onTick = [this] { if (learnAxis >= 0 && ! proc.isLearningVector()) updateVectorButtons(); };
    vectorPad.reset (pad);
    addAndMakeVisible (*vectorPad);
    vectorOn.setButtonText ("VECTOR");
    vectorOn.setClickingTogglesState (true);
    vectorOn.setColour (juce::TextButton::buttonOnColourId, Pal::accent);
    vectorOn.setTooltip (T ("Vector joystick changes the part volumes (like Vector Volume Control on Kronos)"));
    vectorOn.onClick = [this] { auto perf = proc.getPerformance(); perf.vectorOn = vectorOn.getToggleState(); proc.updatePerformance (perf); };
    vectorEqual.setButtonText ("EQ.POW");
    vectorEqual.setClickingTogglesState (true);
    vectorEqual.setColour (juce::TextButton::buttonOnColourId, Pal::accent);
    vectorEqual.setTooltip (T ("Equal Power: crossfade without a level dip in the middle"));
    vectorEqual.onClick = [this] { auto perf = proc.getPerformance(); perf.vectorEqualPower = vectorEqual.getToggleState(); proc.updatePerformance (perf); };
    for (int a = 0; a < 2; ++a)
    {
        auto& b = a == 0 ? vectorCcX : vectorCcY;
        b.setTooltip (T ("Click, then move a control on iPad / Impulse (LEARN)"));
        b.onClick = [this, a] { learnAxis = a; proc.learnVectorCc (a); updateVectorButtons(); };
        addAndMakeVisible (b);
    }
    for (auto* b : { &vectorOn, &vectorEqual, &vectorCcX, &vectorCcY }) { b->getProperties().set ("fontSize", 11.5f); addAndMakeVisible (b); }
    MidiLearn::get().bindButton (vectorOn, "perf.vector.on");
    MidiLearn::get().bindButton (vectorEqual, "perf.vector.equalpower");
    updateVectorButtons();
}

void PerformancePage::updateVectorButtons()
{
    const auto& perf = proc.getPerformance();
    vectorOn.setToggleState (perf.vectorOn, juce::dontSendNotification);
    vectorEqual.setToggleState (perf.vectorEqualPower, juce::dontSendNotification);
    const bool learning = proc.isLearningVector();
    if (! learning) learnAxis = -1;
    vectorCcX.setButtonText (learnAxis == 0 ? T ("move...") : "X: CC " + juce::String (proc.getVectorCc (0)));
    vectorCcY.setButtonText (learnAxis == 1 ? T ("move...") : "Y: CC " + juce::String (proc.getVectorCc (1)));
}

void PerformancePage::refresh()
{
    sceneBar.refresh();
    if (keysButton.getToggleState() != proc.getShowKeyRows())
    {
        keysButton.setToggleState (proc.getShowKeyRows(), juce::dontSendNotification);
        for (auto* s : strips) s->resized();
    }
    if (! nameEdit.isBeingEdited())
        nameEdit.setText (proc.getPerformance().name, juce::dontSendNotification);
    for (auto* s : strips)
        s->refresh();
    updateVectorButtons();
    if (vectorPad != nullptr) vectorPad->repaint();
    for (int b = 0; b < 2; ++b)
    {
        // oznaci stranu na kojoj ima ukljucenih delova
        bool any = false;
        for (int i = b * 8; i < b * 8 + 8; ++i)
            any = any || (proc.getPerformance().parts[(size_t) i].on && proc.getPerformance().parts[(size_t) i].user.isNotEmpty());
        pageButtons[b].setToggleState (b == page, juce::dontSendNotification);
        pageButtons[b].setButtonText (juce::String (b == 0 ? "PARTS 1-8" : "PARTS 9-16") + (any ? juce::String::fromUTF8 (" \xe2\x97\x8f") : juce::String()));
    }
    ranges.repaint();
}

void PerformancePage::paint (juce::Graphics&) {}

void PerformancePage::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (32);
    nameLabel.setBounds (top.removeFromLeft (140));
    nameEdit.setBounds (top.removeFromLeft (300).reduced (0, 2));
    top.removeFromLeft (6);
    saveButton.setBounds (top.removeFromLeft (90).reduced (1, 2));
    pageButtons[1].setBounds (top.removeFromRight (130).reduced (2, 2));
    pageButtons[0].setBounds (top.removeFromRight (130).reduced (2, 2));
    keysButton.setBounds (top.removeFromRight (110).reduced (2, 2));
    top.removeFromLeft (12);
    vectorOn.setBounds (top.removeFromLeft (70).reduced (1, 2));
    vectorEqual.setBounds (top.removeFromLeft (64).reduced (1, 2));
    vectorPad->setBounds (top.removeFromLeft (56).reduced (2, 0));
    vectorCcX.setBounds (top.removeFromLeft (76).reduced (1, 2));
    vectorCcY.setBounds (top.removeFromLeft (76).reduced (1, 2));
    r.removeFromTop (4);
    sceneBar.setBounds (r.removeFromTop (30));

    // sirok ekran (ceo ekran): svih 16 delova odjednom
    const bool all16 = r.getWidth() >= 1440;
    pageButtons[0].setVisible (! all16);
    pageButtons[1].setVisible (! all16);
    if (all16) page = 0;

    r.removeFromTop (4);
    ranges.setBounds (r.removeFromBottom (all16 ? 50 : 78));
    r.removeFromBottom (6);

    const int perPage = all16 ? 16 : 8, gap = all16 ? 4 : 6;
    const int w = (r.getWidth() - gap * (perPage - 1)) / perPage;
    for (int i = 0; i < strips.size(); ++i)
    {
        auto* s = strips[i];
        const bool shown = i / perPage == page;
        s->setVisible (shown);
        if (! shown) continue;
        s->setBounds (r.removeFromLeft (w));
        r.removeFromLeft (gap);
    }
}

//==============================================================================
//  OscRow
//==============================================================================
OscRow::OscRow (LiveSamplerProcessor& p, LiveSamplerEditor& e, int r, std::function<int()> ui, std::function<int()> bf)
    : proc (p), editor (e), row (r), userIndex (std::move (ui)), blockFn (std::move (bf))
{
    onButton.setButtonText ("MS" + juce::String (r + 1));
    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonOnColourId, Pal::accent);
    onButton.getProperties().set ("fontSize", 12.0f);
    onButton.onClick = [this] { edit ([this] (OscSettings& s) { s.on = onButton.getToggleState(); }); };
    addAndMakeVisible (onButton);

    instButton.setColour (juce::TextButton::buttonColourId, Pal::bg);
    instButton.getProperties().set ("fontSize", 13.0f);
    instButton.onClick = [this] { pickInstrument(); };
    instButton.setTooltip ("Choose the multisample (type to search)");
    addAndMakeVisible (instButton);
    instPrev.setButtonText ("-");
    instNext.setButtonText ("+");
    instPrev.setTooltip ("Previous multisample in the library");
    instNext.setTooltip ("Next multisample in the library");
    instPrev.onClick = [this] { stepInstrument (-1); };
    instNext.onClick = [this] { stepInstrument (1); };
    for (auto* b : { &instPrev, &instNext }) { b->getProperties().set ("fontSize", 12.0f); addAndMakeVisible (b); }

    mapButton.setButtonText ("MAP");
    mapButton.getProperties().set ("fontSize", 11.0f);
    mapButton.setTooltip (T ("Open the mapping editor for this instrument"));
    mapButton.onClick = [this] { editor.editMapping (proc.getUser (userIndex()).name, oscIndex()); };
    addAndMakeVisible (mapButton);

    menuButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
    menuButton.setTooltip (T ("Copy / paste / delete this MS"));
    menuButton.onClick = [this] { showMenu(); };
    addAndMakeVisible (menuButton);

    keyOffButton.setButtonText ("KO");
    keyOffButton.setClickingTogglesState (true);
    keyOffButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    keyOffButton.getProperties().set ("fontSize", 11.0f);
    keyOffButton.setTooltip (T ("Key off: this MS plays when you release the key (string release noise, click etc.)"));
    keyOffButton.onClick = [this] { edit ([this] (OscSettings& s) { s.keyOff = keyOffButton.getToggleState(); }); };
    addAndMakeVisible (keyOffButton);

    eqButton.setButtonText ("EQ");
    eqButton.setClickingTogglesState (false);
    eqButton.getProperties().set ("fontSize", 11.0f);
    eqButton.setTooltip (T ("EQ for this MS only"));
    eqButton.onClick = [this] { showEq(); };
    addAndMakeVisible (eqButton);

    auto c3 = [this] { return proc.getC3Is60(); };
    UI::setupIncDec (level, -40, 12, 0.5, [] (double v) { return juce::String (v, 1); });
    UI::setupIncDec (pan, -1, 1, 0.05, UI::panText);
    UI::setupIncDec (transpose, -48, 48, 1, semis);
    UI::setupIncDec (fine, -100, 100, 1, semis);
    UI::setupNoteBox (keyLow, c3);
    UI::setupNoteBox (keyHigh, c3);
    UI::setupIncDec (velLow, 1, 127, 1);
    UI::setupIncDec (velHigh, 1, 127, 1);

    level.onValueChange     = [this] { edit ([this] (OscSettings& s) { s.levelDb = (float) level.getValue(); }); };
    pan.onValueChange       = [this] { edit ([this] (OscSettings& s) { s.pan = (float) pan.getValue(); }); };
    transpose.onValueChange = [this] { edit ([this] (OscSettings& s) { s.transpose = (int) transpose.getValue(); }); };
    fine.onValueChange      = [this] { edit ([this] (OscSettings& s) { s.fineCents = (float) fine.getValue(); }); };
    keyLow.onValueChange    = [this] { edit ([this] (OscSettings& s) { s.keyLow = (int) keyLow.getValue(); s.keyHigh = juce::jmax (s.keyHigh, s.keyLow); }); };
    keyHigh.onValueChange   = [this] { edit ([this] (OscSettings& s) { s.keyHigh = (int) keyHigh.getValue(); s.keyLow = juce::jmin (s.keyLow, s.keyHigh); }); };
    velLow.onValueChange    = [this] { edit ([this] (OscSettings& s) { s.velLow = (int) velLow.getValue(); s.velHigh = juce::jmax (s.velHigh, s.velLow); }); };
    velHigh.onValueChange   = [this] { edit ([this] (OscSettings& s) { s.velHigh = (int) velHigh.getValue(); s.velLow = juce::jmin (s.velLow, s.velHigh); }); };

    for (auto* s : { &level, &pan, &transpose, &fine, &keyLow, &keyHigh, &velLow, &velHigh })
        addAndMakeVisible (s);

    // MIDI Learn: vazi za MS koji se vidi (izabrani user i OSC)
    auto& ml = MidiLearn::get();
    const juce::String id = "user.ms" + juce::String (r + 1) + ".";
    ml.bindButton (onButton, id + "on");
    ml.bindButton (keyOffButton, id + "keyoff");
    ml.bindSlider (level, id + "level");
    ml.bindSlider (pan, id + "pan");
    ml.bindSlider (transpose, id + "transpose");
    ml.bindSlider (fine, id + "fine");
    struct B { const char* n; double lo, hi, c; };
    const B bands[] = { { "eq.low", -15, 15, 0 }, { "eq.mid", -15, 15, 0 }, { "eq.midfreq", 100, 8000, 1000 }, { "eq.high", -15, 15, 0 } };
    for (int k = 0; k < 4; ++k)
        ml.addKnob (id + bands[k].n, bands[k].lo, bands[k].hi, bands[k].c, [this, k] (double v)
        {
            edit ([k, v] (OscSettings& os) { (k == 0 ? os.eqLowDb : k == 1 ? os.eqMidDb : k == 2 ? os.eqMidFreq : os.eqHighDb) = (float) v; });
        });
}

void OscRow::pickInstrument()
{
    const int ui = userIndex();
    if (ui < 0 || ui >= proc.getNumUsers()) return;
    const auto names = proc.getInstrumentList();
    const auto current = proc.getUser (ui).osc[(size_t) oscIndex()].instrument;
    juce::Component::SafePointer<OscRow> safe (this);
    Picker::show (instButton, names, names, { "Browse folders...", "(empty)" }, current, [safe, names, current] (int action, const juce::String& name)
    {
        if (safe == nullptr) return;
        if (action == 0)
        {
            UI::showInstrumentMenu (&safe->instButton, names, current, true, [safe] (const juce::String& n)
            {
                if (safe != nullptr) safe->edit ([n] (OscSettings& s) { s.instrument = n; s.on = n.isNotEmpty(); });
            });
            return;
        }
        const auto n = action == 1 ? juce::String() : name;
        safe->edit ([n] (OscSettings& s) { s.instrument = n; s.on = n.isNotEmpty(); });
    });
}

void OscRow::stepInstrument (int dir)
{
    const int ui = userIndex();
    if (ui < 0 || ui >= proc.getNumUsers()) return;
    const auto names = proc.getInstrumentList();
    if (names.isEmpty()) return;
    const int cur = names.indexOf (proc.getUser (ui).osc[(size_t) oscIndex()].instrument);
    const int next = cur < 0 ? (dir > 0 ? 0 : names.size() - 1) : juce::jlimit (0, names.size() - 1, cur + dir);
    const auto n = names[next];
    edit ([n] (OscSettings& s) { s.instrument = n; s.on = true; });
}

void OscRow::showMenu()
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    juce::PopupMenu m;
    m.addItem (1, T ("Copy MS"));
    m.addItem (2, T ("Paste MS"), Clip::ms.has_value());
    m.addSeparator();
    m.addItem (3, T ("Delete MS"));
    m.addSeparator();
    {
        const auto& cur = proc.getUser (u).osc[(size_t) oscIndex()];
        juce::String state = cur.zoneKey >= 0 ? T ("one sample") : juce::String();
        if (std::abs (cur.keySlope - 1.0f) > 0.001f) state << (state.isEmpty() ? "" : ", ") << (std::abs (cur.keySlope) < 0.005f ? T ("fixed pitch") : juce::String (cur.keySlope, 2) + "x");
        m.addItem (6, T ("Key follow / one sample on all keys...") + (state.isEmpty() ? juce::String() : "   [" + state + "]"));
    }
    {
        const auto& cur = proc.getUser (u).osc[(size_t) oscIndex()];
        const auto layers = cur.on ? proc.velocityLayersOf (cur.instrument) : std::vector<int>();
        if (layers.size() > 1)
        {
            m.addSeparator();
            m.addSectionHeader (T ("Velocity layers (") + juce::String ((int) layers.size()) + ")");
            m.addItem (4, T ("Split layers to MS1-MS") + juce::String ((int) layers.size()));
            m.addItem (5, T ("Delete this velocity layer from disk (") + juce::String (cur.velLow) + "-" + juce::String (cur.velHigh) + ")...");
        }
    }
    juce::Component::SafePointer<OscRow> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 6) { safe->showKeyFollow(); return; }
        auto info = [] (const juce::String& t) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("Velocity layers"), t); };
        if (r == 4)
        {
            juce::String rep;
            safe->proc.splitVelocityLayers (safe->userIndex(), safe->oscIndex(), rep);
            info (rep);
            return;
        }
        if (r == 5)
        {
            const auto& o = safe->proc.getUser (safe->userIndex()).osc[(size_t) safe->oscIndex()];
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("Delete velocity layer"),
                T ("Permanently deleting from disk all samples of this layer (velocity ") + juce::String (o.velLow) + "-" + juce::String (o.velHigh)
                    + T (") for:\n") + o.instrument + T ("\n\nThe neighbouring layer takes over its range. This applies to all users that use this sound."),
                T ("Delete"), T ("Cancel"), nullptr,
                juce::ModalCallbackFunction::create ([safe, info] (int ok)
                {
                    if (safe == nullptr || ok == 0) return;
                    juce::String rep;
                    safe->proc.deleteVelocityLayer (safe->userIndex(), safe->oscIndex(), rep);
                    info (rep);
                }));
            return;
        }
        const auto& cur = safe->proc.getUser (safe->userIndex()).osc[(size_t) safe->oscIndex()];
        if (r == 1) Clip::ms = cur;
        if (r == 2 && Clip::ms) safe->edit ([] (OscSettings& s) { const int b = s.block; s = *Clip::ms; s.block = b; });
        if (r == 3) safe->edit ([] (OscSettings& s) { const int b = s.block; s = OscSettings(); s.block = b; });
    });
}

namespace
{
    // EQ jednog MS-a: bas, sredina (sa frekvencijom), visoki
    struct MsEqPanel : public juce::Component
    {
        MsEqPanel (LiveSamplerProcessor& p, int user, int osc, juce::Colour c) : proc (p), u (user), o (osc)
        {
            auto db = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
            const auto& s = proc.getUser (u).osc[(size_t) o];
            struct K { const char* n; double lo, hi, centre, def, val; std::function<juce::String (double)> f; };
            const K ks[] = { { "Low", -15, 15, 0, 0, s.eqLowDb, db }, { "Mid", -15, 15, 0, 0, s.eqMidDb, db },
                             { "Mid Hz", 100, 8000, 1000, 1000, s.eqMidFreq, UI::hzText }, { "High", -15, 15, 0, 0, s.eqHighDb, db } };
            for (int k = 0; k < 4; ++k)
            {
                auto* kn = knobs.add (new ValueKnob (T (ks[k].n), c, ks[k].lo, ks[k].hi, ks[k].centre, ks[k].f, ks[k].def));
                kn->setValue (ks[k].val);
                const char* bands[] = { "eq.low", "eq.mid", "eq.midfreq", "eq.high" };
                kn->slider.setLearnId ("user.ms" + juce::String (o % kMsPerOsc + 1) + "." + bands[k]);
                kn->onChange = [this, k] (double v)
                {
                    auto user = proc.getUser (u);
                    auto& os = user.osc[(size_t) o];
                    (k == 0 ? os.eqLowDb : k == 1 ? os.eqMidDb : k == 2 ? os.eqMidFreq : os.eqHighDb) = (float) v;
                    proc.updateUser (u, user);
                };
                addAndMakeVisible (kn);
            }
            setSize (360, 110);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (6);
            const int w = r.getWidth() / 4;
            for (auto* k : knobs) k->setBounds (r.removeFromLeft (w));
        }
        LiveSamplerProcessor& proc;
        const int u, o;
        juce::OwnedArray<ValueKnob> knobs;
    };
}

namespace
{
    // Key follow (Pitch Slope) i "jedan sempl na sve tastere" za jedan MS
    struct MsKeyFollowPanel : public juce::Component
    {
        MsKeyFollowPanel (LiveSamplerProcessor& p, int user, int osc, juce::Colour c)
            : proc (p), u (user), o (osc),
              slope (T ("Key follow"), c, -1.0, 2.0, 1.0, [] (double v) { return std::abs (v) < 0.005 ? juce::String ("Fixed") : juce::String (v, 2) + "x"; }, 1.0)
        {
            const auto& s = proc.getUser (u).osc[(size_t) o];
            slope.setValue (s.keySlope);
            slope.slider.setLearnId ("user.ms" + juce::String (o % kMsPerOsc + 1) + ".keyfollow");
            slope.onChange = [this] (double v) { edit ([v] (OscSettings& x) { x.keySlope = (float) (std::abs (v) < 0.02 ? 0.0 : v); }); };
            addAndMakeVisible (slope);

            one.setButtonText (T ("One sample on all keys, taken from key:"));
            one.setToggleState (s.zoneKey >= 0, juce::dontSendNotification);
            one.onClick = [this] { const bool b = one.getToggleState(); const int k = (int) key.getValue(); edit ([b, k] (OscSettings& x) { x.zoneKey = b ? k : -1; }); };
            addAndMakeVisible (one);

            UI::setupNoteBox (key, [this] { return proc.getC3Is60(); });
            key.setValue (s.zoneKey >= 0 ? s.zoneKey : juce::jlimit (0, 127, (s.keyLow + s.keyHigh) / 2), juce::dontSendNotification);
            key.onValueChange = [this] { if (! one.getToggleState()) return; const int k = (int) key.getValue(); edit ([k] (OscSettings& x) { x.zoneKey = k; }); };
            addAndMakeVisible (key);

            drum.setButtonText (T ("Drum hit: same sample, same pitch on all keys"));
            drum.onClick = [this]
            {
                const int k = (int) key.getValue();
                one.setToggleState (true, juce::dontSendNotification);
                slope.setValue (0.0);
                edit ([k] (OscSettings& x) { x.zoneKey = k; x.keySlope = 0.0f; });
            };
            addAndMakeVisible (drum);
            normal.setButtonText (T ("Normal (each key its own zone, full key follow)"));
            normal.onClick = [this]
            {
                one.setToggleState (false, juce::dontSendNotification);
                slope.setValue (1.0);
                edit ([] (OscSettings& x) { x.zoneKey = -1; x.keySlope = 1.0f; });
            };
            addAndMakeVisible (normal);

            help.setText (T ("Key follow 1.00x = normal pitch, 0 = Fixed (the same pitch on every key, e.g. a drum hit), 0.50x = a semitone every 2 keys, "
                             "negative = pitch goes down. The pivot is the root key of the sample. The MS key range (Key low / high) decides on which keys it plays."),
                          juce::dontSendNotification);
            help.setFont (juce::FontOptions (12.0f));
            help.setColour (juce::Label::textColourId, Pal::dim);
            addAndMakeVisible (help);
            setSize (430, 250);
        }
        void edit (std::function<void (OscSettings&)> fn)
        {
            auto user = proc.getUser (u);
            fn (user.osc[(size_t) o]);
            proc.updateUser (u, user);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto top = r.removeFromTop (100);
            slope.setBounds (top.removeFromLeft (100));
            top.removeFromLeft (8);
            one.setBounds (top.removeFromTop (28));
            key.setBounds (top.removeFromTop (30).removeFromLeft (120).withX (one.getX() + 24));
            key.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 30);
            r.removeFromTop (4);
            drum.setBounds (r.removeFromTop (30));
            r.removeFromTop (4);
            normal.setBounds (r.removeFromTop (30));
            r.removeFromTop (4);
            help.setBounds (r);
        }
        LiveSamplerProcessor& proc;
        const int u, o;
        ValueKnob slope;
        juce::ToggleButton one;
        LearnSlider key;
        juce::TextButton drum, normal;
        juce::Label help;
    };
}

void OscRow::showKeyFollow()
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    auto* top = getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::make_unique<MsKeyFollowPanel> (proc, u, oscIndex(), Pal::part[blockFn()]),
                                            top->getLocalArea (this, menuButton.getBounds()), top);
}

void OscRow::showEq()
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    auto* top = getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::make_unique<MsEqPanel> (proc, u, oscIndex(), Pal::part[blockFn()]),
                                            top->getLocalArea (this, eqButton.getBounds()), top);
}

void OscRow::edit (std::function<void (OscSettings&)> fn)
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    auto user = proc.getUser (u);
    fn (user.osc[(size_t) oscIndex()]);
    proc.updateUser (u, user);
}

void OscRow::refresh()
{
    const int u = userIndex();
    const bool valid = u >= 0 && u < proc.getNumUsers();
    setEnabled (valid);
    if (! valid) return;

    const auto& s = proc.getUser (u).osc[(size_t) oscIndex()];
    onButton.setToggleState (s.on, juce::dontSendNotification);
    auto instText = s.instrument.isEmpty() ? T ("— instrument —") : s.instrument.fromFirstOccurrenceOf ("/", false, false);
    if (s.instrument.isNotEmpty() && ! proc.getInstrumentList().contains (s.instrument))
        instText = T ("⚠ missing: ") + s.instrument;
    instButton.setButtonText (instText);
    mapButton.setEnabled (s.instrument.isNotEmpty());
    onButton.setColour (juce::TextButton::buttonOnColourId, Pal::part[blockFn()]);
    keyOffButton.setToggleState (s.keyOff, juce::dontSendNotification);
    {
        // key follow / jedan sempl: narandzasta strelica na meniju MS-a
        const bool kf = s.zoneKey >= 0 || std::abs (s.keySlope - 1.0f) > 0.001f;
        if (kf) menuButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffff8a3d));
        else menuButton.removeColour (juce::TextButton::buttonColourId);
        menuButton.setTooltip (kf ? T ("Key follow: ") + (std::abs (s.keySlope) < 0.005f ? T ("Fixed") : juce::String (s.keySlope, 2) + "x")
                                        + (s.zoneKey >= 0 ? T (", one sample from key ") + juce::MidiMessage::getMidiNoteName (s.zoneKey, true, true, proc.getC3Is60() ? 3 : 4) : juce::String())
                                  : T ("Copy / paste / delete this MS, key follow"));
    }
    eqButton.setColour (juce::TextButton::buttonColourId, s.hasEq() ? juce::Colour (0xff2f7d4a) : Pal::panelHi);
    setSliderValue (level, s.levelDb);
    setSliderValue (pan, s.pan);
    setSliderValue (transpose, s.transpose);
    setSliderValue (fine, s.fineCents);
    setSliderValue (keyLow, s.keyLow);
    setSliderValue (keyHigh, s.keyHigh);
    setSliderValue (velLow, s.velLow);
    setSliderValue (velHigh, s.velHigh);
    keyLow.updateText();
    keyHigh.updateText();

    for (auto* c : std::initializer_list<juce::Component*> { &instButton, &level, &pan, &transpose, &fine, &keyLow, &keyHigh, &velLow, &velHigh })
        c->setAlpha (s.on ? 1.0f : 0.5f);
}

void OscRow::paint (juce::Graphics& g)
{
    g.setColour (row % 2 == 0 ? Pal::panel : Pal::panel.brighter (0.03f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
}

void OscRow::resized()
{
    auto r = getLocalBounds().reduced (4, 3);
    onButton.setBounds (r.removeFromLeft (46));
    menuButton.setBounds (r.removeFromLeft (18));
    r.removeFromLeft (6);
    const int boxW = 82;
    juce::Slider* boxes[] = { &level, &pan, &transpose, &fine, &keyLow, &keyHigh, &velLow, &velHigh };
    const int fixed = boxW * 8 + 56 + 6 + 72;
    auto instArea = r.removeFromLeft (r.getWidth() - fixed);
    instPrev.setBounds (instArea.removeFromLeft (20).reduced (0, 2));
    instNext.setBounds (instArea.removeFromRight (20).reduced (0, 2));
    instButton.setBounds (instArea.reduced (1, 0));
    r.removeFromLeft (4);
    mapButton.setBounds (r.removeFromLeft (52));
    r.removeFromLeft (2);
    keyOffButton.setBounds (r.removeFromLeft (34).reduced (1, 0));
    eqButton.setBounds (r.removeFromLeft (34).reduced (1, 0));
    r.removeFromLeft (4);
    for (auto* b : boxes)
    {
        b->setBounds (r.removeFromLeft (boxW).reduced (2, 0));
        b->setTextBoxStyle (juce::Slider::TextBoxLeft, false, boxW - 38, r.getHeight());
    }
}

//==============================================================================
//  EnvelopeView
//==============================================================================
void EnvelopeView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::lcdBg);
    g.fillRoundedRectangle (r, 6.0f);
    auto a = r.reduced (10.0f, 12.0f);

    // vremena kao udeo sirine (sustain dobija fiksni deo)
    auto tw = [] (float t) { return std::sqrt (juce::jmax (0.0f, t)) + 0.05f; };
    const float ta = tw (env.attackTime), td = tw (env.decayTime), ts = tw (env.slopeTime), tr = tw (env.releaseTime);
    const float sus = 0.6f;
    const float total = ta + td + ts + sus + tr;
    auto yOf = [&] (float lvl)
    {
        const float norm = bip ? (lvl + 1.0f) * 0.5f : lvl;
        return a.getBottom() - juce::jlimit (0.0f, 1.0f, norm) * a.getHeight();
    };

    if (bip)
    {
        g.setColour (Pal::lcdText.withAlpha (0.4f));
        g.drawHorizontalLine (juce::roundToInt (yOf (0.0f)), a.getX(), a.getRight());
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("0", juce::Rectangle<float> (a.getX() - 9.0f, yOf (0.0f) - 6.0f, 8.0f, 12.0f), juce::Justification::centredRight);
    }

    float x = a.getX();
    const float w = a.getWidth() / total;
    juce::Path path;
    path.startNewSubPath (x, yOf (env.start));
    x += ta * w; path.lineTo (x, yOf (env.attackLevel));
    x += td * w; path.lineTo (x, yOf (env.breakLevel));
    x += ts * w; path.lineTo (x, yOf (env.sustain));
    x += sus * w; path.lineTo (x, yOf (env.sustain));
    const float noteOffX = x;
    x += tr * w; path.lineTo (x, yOf (bip ? env.releaseLevel : 0.0f));

    g.setColour (colour.withAlpha (0.18f));
    juce::Path fill (path);
    fill.lineTo (x, yOf (bip ? 0.0f : 0.0f));
    fill.lineTo (a.getX(), yOf (0.0f));
    fill.closeSubPath();
    g.fillPath (fill);
    g.setColour (colour);
    g.strokePath (path, juce::PathStrokeType (2.0f));

    g.setColour (Pal::lcdText.withAlpha (0.35f));
    const float dashes[] = { 3.0f, 3.0f };
    g.drawDashedLine (juce::Line<float> (noteOffX, a.getY(), noteOffX, a.getBottom()), dashes, 2);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("note off", juce::Rectangle<float> (noteOffX + 3.0f, a.getY() - 10.0f, 60.0f, 12.0f), juce::Justification::centredLeft);
}

//==============================================================================
//  SoundEditor
//==============================================================================
static juce::String levelText (double v)  { return juce::String (juce::roundToInt (v * 100.0)); }
static juce::String centsText (double v)  { return juce::String (juce::roundToInt (v)) + " c"; }
static juce::String octText (double v)    { return (v > 0 ? "+" : "") + juce::String (v, 1) + " oct"; }
static juce::String eqDb (double v)       { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; }
static juce::String hzText2 (double v)    { return juce::String (v, 2) + " Hz"; }
static juce::String stText (double v)     { return (v > 0 ? "+" : "") + juce::String (v, 1) + " st"; }

static std::vector<std::vector<SoundEditor::Param>> sectionParams (int section);
static juce::String sectionKey (int section);

SoundEditor::SoundEditor (LiveSamplerProcessor& p, std::function<int()> ui)
    : proc (p), userIndex (std::move (ui))
{
    for (int b = 0; b < kNumBlocks; ++b)
    {
        blockButtons[b].setButtonText ("OSC" + juce::String (b + 1));
        blockButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::part[b]);
        blockButtons[b].getProperties().set ("fontSize", 12.0f);
        blockButtons[b].setTooltip (T ("Select OSC: its MS1-MS8 are shown above, its sound below. Kronos uses OSC1 and OSC2."));
        blockButtons[b].onClick = [this, b] { block = b; rebuildKnobs(); if (onBlockChanged) onBlockChanged(); };
        addAndMakeVisible (blockButtons[b]);
    }
    blockMenu.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
    blockMenu.setTooltip (T ("Copy / paste / delete the whole OSC (sound and all 8 MS)"));
    blockMenu.onClick = [this] { showBlockMenu(); };
    addAndMakeVisible (blockMenu);

    // MONO / PRELAZ
    auto styleL = [] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, Pal::dim);
    };
    styleL (monoLabel, T ("MONO RANGE (outside the range: poly, no portamento)"));
    styleL (articLabel, T ("ARTICULATION: on which intervals (semitones) this OSC is triggered"));
    articHelp.setText (T ("Turn ARTICULATION on for several OSCs. The OSC without selected intervals plays the first note. While you hold a note and play "
                          "the next one, the OSC whose interval matches is triggered and the previous one stops (like a string slide over frets). "
                          "Works when the user is Mono / Mono legato."), juce::dontSendNotification);
    articHelp.setFont (juce::FontOptions (12.0f));
    articHelp.setColour (juce::Label::textColourId, Pal::dim);
    articHelp.setJustificationType (juce::Justification::topLeft);
    auto c3 = [this] { return proc.getC3Is60(); };
    UI::setupNoteBox (monoLow, c3);
    UI::setupNoteBox (monoHigh, c3);
    monoLow.onValueChange = [this] { const int v = (int) monoLow.getValue(); const int b = block; edit ([v, b] (UserProgram& u) { auto& s = u.block[(size_t) b]; s.monoLow = v; s.monoHigh = juce::jmax (s.monoHigh, v); }); };
    monoHigh.onValueChange = [this] { const int v = (int) monoHigh.getValue(); const int b = block; edit ([v, b] (UserProgram& u) { auto& s = u.block[(size_t) b]; s.monoHigh = v; s.monoLow = juce::jmin (s.monoLow, v); }); };
    articOn.setButtonText (T ("ARTIC"));
    articOn.setClickingTogglesState (true);
    articOn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    articOn.onClick = [this] { const bool v = articOn.getToggleState(); const int b = block; edit ([v, b] (UserProgram& u) { u.block[(size_t) b].articOn = v; }); };
    articDir.addItemList ({ T ("Up and down"), T ("Up only"), T ("Down only") }, 1);
    articDir.onChange = [this] { const int v = articDir.getSelectedItemIndex(); const int b = block; edit ([v, b] (UserProgram& u) { u.block[(size_t) b].articDir = v; }); };
    const char* iv[] = { "minor second", "major second", "minor third", "major third", "fourth", "tritone", "fifth",
                         "minor sixth", "major sixth", "minor seventh", "major seventh", "octave" };
    for (int i = 0; i < 24; ++i)
    {
        auto& b = intervalButtons[i];
        b.setButtonText (juce::String (i + 1));
        b.setTooltip (juce::String (i + 1) + T (" semitone(s)") + (i < 12 ? juce::String (" - ") + T (iv[i]) : (i == 23 ? T (" - two octaves") : T (" - over an octave"))));
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
        b.getProperties().set ("fontSize", 11.0f);
        b.onClick = [this, i]
        {
            const bool on = intervalButtons[i].getToggleState();
            const int bl = block;
            edit ([on, i, bl] (UserProgram& u) { auto& m = u.block[(size_t) bl].articMask; m = on ? (m | (1 << i)) : (m & ~(1 << i)); });
        };
    }
    for (auto* c : std::initializer_list<juce::Component*> { &monoLabel, &articLabel, &articHelp, &monoLow, &monoHigh, &articOn, &articDir })
        addChildComponent (c);
    for (auto& b : intervalButtons)
        addChildComponent (b);

    const char* secNames[] = { "PITCH", "FILTER", "AMP", "MONO / ARTIC", "COMMON", "EFFECT / EQ" };
    for (int s = 0; s < NumSections; ++s)
    {
        sectionButtons[s].setButtonText (T (secNames[s]));
        sectionButtons[s].setColour (juce::TextButton::buttonOnColourId, Pal::lcdText);
        sectionButtons[s].onClick = [this, s] { showSection (s); };
        addAndMakeVisible (sectionButtons[s]);
    }
    addAndMakeVisible (envView);

    filterType.addItemList ({ T ("Off"), "Lowpass 24 dB", "Lowpass 12 dB", "Highpass", "Bandpass" }, 1);
    filterType.onChange = [this] { const int t = filterType.getSelectedItemIndex(); const int b = block; edit ([t, b] (UserProgram& u) { u.block[(size_t) b].filterType = t; }); };
    filterLabel.setText ("FILTER TYPE", juce::dontSendNotification);
    filterLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    filterLabel.setColour (juce::Label::textColourId, Pal::dim);
    addChildComponent (filterType);
    addChildComponent (filterLabel);

    voiceMode.addItemList ({ "Poly", "Mono", "Mono legato", "Mono legato crossfade" }, 1);
    voiceMode.setTooltip (T ("Mono legato: the first note keeps playing and only its pitch changes. "
                             "Mono legato crossfade: each new note plays the sample of its own key zone, crossfaded from the previous note without a new attack (more realistic for sax, strings, voice)."));
    voiceMode.onChange = [this] { const int v = voiceMode.getSelectedItemIndex(); edit ([v] (UserProgram& u) { u.voiceMode = v; }); refreshLegatoKnobs(); };
    priority.addItemList ({ T ("Last note"), T ("Highest note"), T ("Lowest note") }, 1);
    priority.onChange = [this] { const int v = priority.getSelectedItemIndex(); edit ([v] (UserProgram& u) { u.priority = v; }); };
    for (auto* b : { &portaOn, &portaFingered, &portaConstRate })
    {
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        b->getProperties().set ("fontSize", 13.0f);
    }
    portaOn.setButtonText ("PORTAMENTO");
    portaFingered.setButtonText ("Fingered only");
    portaConstRate.setButtonText ("Constant rate");
    portaConstRate.setTooltip (T ("A bigger jump takes longer (time is per octave)"));
    portaOn.onClick = [this] { const bool v = portaOn.getToggleState(); edit ([v] (UserProgram& u) { u.portaOn = v; }); };
    portaFingered.onClick = [this] { const bool v = portaFingered.getToggleState(); edit ([v] (UserProgram& u) { u.portaFingered = v; }); };
    portaConstRate.onClick = [this] { const bool v = portaConstRate.getToggleState(); edit ([v] (UserProgram& u) { u.portaConstRate = v; }); };
    portaTime = std::make_unique<ValueKnob> ("Time", Pal::accent, 0.001, 3.0, 0.3, UI::timeText, 0.1);
    portaTime->onChange = [this] (double v) { edit ([v] (UserProgram& u) { u.portamento = (float) v; }); };
    legatoXfade = std::make_unique<ValueKnob> ("Legato xfade", Pal::accent, 0.005, 0.5, 0.06, UI::timeText, 0.06);
    legatoXfade->onChange = [this] (double v) { edit ([v] (UserProgram& u) { u.legatoXfade = (float) v; }); };
    legatoXfade->slider.setTooltip (T ("Crossfade time from the previous note into the new one"));
    legatoStart = std::make_unique<ValueKnob> ("Skip attack", Pal::accent, 0.0, 0.5, 0.05, UI::timeText, 0.05);
    legatoStart->onChange = [this] (double v) { edit ([v] (UserProgram& u) { u.legatoStart = (float) v; }); };
    legatoStart->slider.setTooltip (T ("The new note's sample starts this far in, so its attack is not heard (legato)"));
    addChildComponent (legatoXfade.get());
    addChildComponent (legatoStart.get());
    UI::setupIncDec (bendUp, 0, 24, 1);
    UI::setupIncDec (bendDown, 0, 24, 1);
    bendUp.onValueChange = [this] { const int v = (int) bendUp.getValue(); edit ([v] (UserProgram& u) { u.bendUp = v; }); };
    bendDown.onValueChange = [this] { const int v = (int) bendDown.getValue(); edit ([v] (UserProgram& u) { u.bendDown = v; }); };
    const char* cl[] = { "VOICE", "PRIORITY", "BEND UP", "BEND DOWN" };
    for (int i = 0; i < 4; ++i)
    {
        commonLabels[i].setText (T (cl[i]), juce::dontSendNotification);
        commonLabels[i].setFont (juce::FontOptions (12.0f, juce::Font::bold));
        commonLabels[i].setColour (juce::Label::textColourId, Pal::dim);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &voiceMode, &priority, &portaOn, &portaFingered, &portaConstRate,
                                                             portaTime.get(), &bendUp, &bendDown, &commonLabels[0], &commonLabels[1],
                                                             &commonLabels[2], &commonLabels[3] })
        addChildComponent (c);

    fxType.addItemList (InsertFx::getTypeNames(), 1);
    fxType.onChange = [this] { const int v = fxType.getSelectedItemIndex(); edit ([v] (UserProgram& u) { u.fxType = v; }); };
    fxLabel.setText ("INSERT EFFECT", juce::dontSendNotification);
    fxLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    fxLabel.setColour (juce::Label::textColourId, Pal::dim);
    addChildComponent (fxType);
    addChildComponent (fxLabel);

    // OSC on/off (svih 8 MS ovog OSC-a)
    oscOnButton.setButtonText ("ON");
    oscOnButton.setClickingTogglesState (true);
    oscOnButton.getProperties().set ("fontSize", 12.0f);
    oscOnButton.setTooltip ("Selected OSC on / off (all its MS)");
    oscOnButton.onClick = [this] { const bool v = oscOnButton.getToggleState(); const int b = block; edit ([v, b] (UserProgram& u) { u.block[(size_t) b].on = v; }); };
    oscOnButton.learnId = [this] { return "user.osc" + juce::String (block + 1) + ".on"; };
    addAndMakeVisible (oscOnButton);

    // MIDI Learn: vazi za izabranog usera; parametri zvuka za izabrani OSC
    auto& ml = MidiLearn::get();
    for (int b = 0; b < kNumBlocks; ++b)
        ml.addToggle ("user.osc" + juce::String (b + 1) + ".on",
                      [this, b] { const int ui2 = userIndex(); return ui2 >= 0 && ui2 < proc.getNumUsers() && proc.getUser (ui2).block[(size_t) b].on; },
                      [this, b] (bool v) { edit ([v, b] (UserProgram& u) { u.block[(size_t) b].on = v; }); });
    for (int sec : { (int) Pitch, (int) Filter, (int) Amp, (int) Effect })
    {
        const auto prs = sectionParams (sec);
        for (size_t ri = 0; ri < prs.size(); ++ri)
            for (size_t pi = 0; pi < prs[ri].size(); ++pi)
            {
                const auto& prm = prs[ri][pi];
                auto ref = prm.ref;
                ml.addKnob ("user." + sectionKey (sec) + "." + juce::String ((int) ri) + "." + juce::String ((int) pi), prm.lo, prm.hi, prm.centre,
                            [this, ref] (double v) { const int b = block; edit ([ref, b, v] (UserProgram& u) { ref (u, b) = (float) v; }); });
            }
    }
    ml.bindCombo (filterType, "user.filter.type");
    ml.bindCombo (voiceMode, "user.common.voice");
    ml.bindCombo (priority, "user.common.priority");
    ml.bindButton (portaOn, "user.common.portamento");
    ml.bindButton (portaFingered, "user.common.fingered");
    ml.bindButton (portaConstRate, "user.common.constrate");
    ml.bindSlider (portaTime->slider, "user.common.portatime");
    ml.bindSlider (legatoXfade->slider, "user.common.legatoxfade");
    ml.bindSlider (legatoStart->slider, "user.common.legatostart");
    ml.bindSlider (bendUp, "user.common.bendup");
    ml.bindSlider (bendDown, "user.common.benddown");
    ml.bindCombo (fxType, "user.effect.type");
    ml.bindButton (articOn, "user.mono.artic");
    ml.bindSlider (monoLow, "user.mono.low");
    ml.bindSlider (monoHigh, "user.mono.high");

    showSection (Filter);
}

void SoundEditor::edit (std::function<void (UserProgram&)> fn)
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    auto user = proc.getUser (u);
    fn (user);
    proc.updateUser (u, user);
}

void SoundEditor::showSection (int s)
{
    section = s;
    rebuildKnobs();
}

static std::vector<SoundEditor::Param> envParams (std::function<EnvSettings& (UserProgram&, int)> env, bool bipolar, bool withReleaseLevel);

static std::vector<std::vector<SoundEditor::Param>> sectionParams (int section)
{
    using SE = SoundEditor;
    std::vector<std::vector<SE::Param>> rows;
    auto P = [] (const char* n, double lo, double hi, double c, double d, std::function<juce::String (double)> f,
                 std::function<float& (UserProgram&, int)> r) { return SE::Param { T (n), lo, hi, c, d, std::move (f), std::move (r) }; };
    using U = UserProgram;
    enum { Pitch = SE::Pitch, Filter = SE::Filter, Amp = SE::Amp, Effect = SE::Effect };

    switch (section)
    {
        case Pitch:
            rows.push_back ({
                P ("EG int.", -12, 12, 0, 0, stText, [] (U& u, int b) -> float& { return u.block[(size_t) b].pitchEgInt; }),
                P ("Vibrato rate", 0.1, 12, 4, 5.5, hzText2, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibRate; }),
                P ("Vibrato depth", 0, 300, 50, 0, centsText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibDepth; }),
                P ("Delay", 0, 5, 0.5, 0.3, UI::timeText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibDelay; }),
                P ("Fade", 0, 5, 0.5, 0.25, UI::timeText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibFade; }),
                P ("Offset", -1, 1, 0, 0, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibOffset; }),
                P ("ModWh→vib", 0, 300, 50, 30, centsText, [] (U& u, int b) -> float& { return u.block[(size_t) b].mwToVib; }),
                P ("Aftert→vib", 0, 100, 0, 0, centsText, [] (U& u, int b) -> float& { return u.block[(size_t) b].atToVib; }) });
            rows.push_back (envParams ([] (U& u, int b) -> EnvSettings& { return u.block[(size_t) b].pitchEg; }, true, true));
            break;

        case Filter:
            rows.push_back ({
                P ("Cutoff", 40, 20000, 1500, 20000, UI::hzText, [] (U& u, int b) -> float& { return u.block[(size_t) b].cutoff; }),
                P ("Resonance", 0, 1, 0, 0, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].resonance; }),
                P ("Key track", 0, 2, 0, 0, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].keyTrack; }),
                P ("Velocity", -4, 4, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].velToCutoff; }),
                P ("EG int.", -10, 10, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].filterEgInt; }),
                P ("Vel→EG", -10, 10, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].velToFilterEg; }),
                P ("Bend→cut", -6, 6, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].bendToCutoff; }),
                P ("LFO rate", 0.05, 15, 3, 2, hzText2, [] (U& u, int b) -> float& { return u.block[(size_t) b].filterLfoRate; }),
                P ("LFO depth", 0, 3, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].filterLfoDepth; }),
                P ("ModWh→cut", -4, 4, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].mwToCutoff; }),
                P ("Aftert→cut", -4, 4, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].atToCutoff; }) });
            rows.push_back (envParams ([] (U& u, int b) -> EnvSettings& { return u.block[(size_t) b].filterEg; }, true, true));
            break;

        case Amp:
            rows.push_back ({
                P ("Velocity", 0, 1, 0, 0.8, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].velSens; }),
                P ("Vel curve", 0.3, 4, 1.4, 1.6, [] (double v) { return juce::String (v, 2); }, [] (U& u, int b) -> float& { return u.block[(size_t) b].velCurve; }),
                P ("Aftert→amp", 0, 12, 0, 0, [] (double v) { return "+" + juce::String (v, 1) + " dB"; }, [] (U& u, int b) -> float& { return u.block[(size_t) b].atToAmp; }) });
            rows.push_back (envParams ([] (U& u, int b) -> EnvSettings& { return u.block[(size_t) b].ampEg; }, false, false));
            break;

        case Effect:
            rows.push_back ({
                P ("FX rate", 0, 1, 0, 0.3, UI::pctText, [] (U& u, int) -> float& { return u.fxRate; }),
                P ("FX depth", 0, 1, 0, 0.5, UI::pctText, [] (U& u, int) -> float& { return u.fxDepth; }),
                P ("FX mix", 0, 1, 0, 0.5, UI::pctText, [] (U& u, int) -> float& { return u.fxMix; }),
                P ("Send reverb", 0, 1, 0, 1, UI::pctText, [] (U& u, int) -> float& { return u.send1; }),
                P ("Send delay", 0, 1, 0, 1, UI::pctText, [] (U& u, int) -> float& { return u.send2; }),
                P ("EQ low", -15, 15, 0, 0, eqDb, [] (U& u, int) -> float& { return u.eqLowDb; }),
                P ("EQ mid", -15, 15, 0, 0, eqDb, [] (U& u, int) -> float& { return u.eqMidDb; }),
                P ("Mid Hz", 100, 8000, 1000, 1000, UI::hzText, [] (U& u, int) -> float& { return u.eqMidFreq; }),
                P ("EQ high", -15, 15, 0, 0, eqDb, [] (U& u, int) -> float& { return u.eqHighDb; }) });
            break;

        default:
            break;
    }

    return rows;
}

static juce::String sectionKey (int section)
{
    const char* k[] = { "pitch", "filter", "amp", "mono", "common", "effect" };
    return k[juce::jlimit (0, 5, section)];
}

void SoundEditor::rebuildKnobs()
{
    knobs.clear();
    rows = sectionParams (section);
    const auto acc = Pal::part[block];

    for (size_t ri = 0; ri < rows.size(); ++ri)
        for (size_t pi = 0; pi < rows[ri].size(); ++pi)
        {
            auto& prm = rows[ri][pi];
            auto* k = knobs.add (new ValueKnob (prm.name, acc, prm.lo, prm.hi, prm.centre, prm.fmt, prm.def));
            k->slider.setLearnId ("user." + sectionKey (section) + "." + juce::String ((int) ri) + "." + juce::String ((int) pi));
            auto ref = prm.ref;
            k->onChange = [this, ref] (double v)
            {
                const int b = block;
                edit ([ref, b, v] (UserProgram& u) { ref (u, b) = (float) v; });
            };
            addAndMakeVisible (k);
        }

    const bool isBlock = section == Pitch || section == Filter || section == Amp;
    for (int b = 0; b < kNumBlocks; ++b)
        blockButtons[b].setToggleState (b == block, juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &monoLabel, &articLabel, &articHelp, &monoLow, &monoHigh, &articOn, &articDir })
        c->setVisible (section == MonoArtic);
    for (auto& b : intervalButtons)
        b.setVisible (section == MonoArtic);
    for (int s = 0; s < NumSections; ++s)
        sectionButtons[s].setToggleState (s == section, juce::dontSendNotification);

    envView.setVisible (isBlock);
    filterType.setVisible (section == Filter);
    filterLabel.setVisible (section == Filter);
    for (auto* c : std::initializer_list<juce::Component*> { &voiceMode, &priority, &portaOn, &portaFingered, &portaConstRate,
                                                             portaTime.get(), &bendUp, &bendDown, &commonLabels[0], &commonLabels[1],
                                                             &commonLabels[2], &commonLabels[3] })
        c->setVisible (section == Common);
    refreshLegatoKnobs();
    fxType.setVisible (section == Effect);
    fxLabel.setVisible (section == Effect);

    resized();
    refresh();
}

static std::vector<SoundEditor::Param> envParams (std::function<EnvSettings& (UserProgram&, int)> env, bool bipolar, bool withReleaseLevel)
{
    using P = SoundEditor::Param;
    const double lo = bipolar ? -1.0 : 0.0;
    std::vector<P> v;
    auto lvl = [&] (const char* n, double def, float EnvSettings::* m)
    {
        v.push_back ({ T (n), lo, 1.0, 0.0, def, levelText, [env, m] (UserProgram& u, int b) -> float& { return env (u, b).*m; } });
    };
    auto tim = [&] (const char* n, double def, float EnvSettings::* m)
    {
        v.push_back ({ T (n), 0.001, 10.0, 0.6, def, UI::timeText, [env, m] (UserProgram& u, int b) -> float& { return env (u, b).*m; } });
    };
    lvl ("Start", 0.0, &EnvSettings::start);
    tim ("Attack", 0.001, &EnvSettings::attackTime);
    lvl ("Attack lvl", 1.0, &EnvSettings::attackLevel);
    tim ("Decay", 0.3, &EnvSettings::decayTime);
    lvl ("Break lvl", 1.0, &EnvSettings::breakLevel);
    tim ("Slope", 0.3, &EnvSettings::slopeTime);
    lvl ("Sustain", 1.0, &EnvSettings::sustain);
    tim ("Release", 0.3, &EnvSettings::releaseTime);
    if (withReleaseLevel)
        lvl ("Release lvl", 0.0, &EnvSettings::releaseLevel);
    return v;
}

const EnvSettings* SoundEditor::currentEnv (const UserProgram& u, bool& bipolar) const
{
    const auto& sb = u.block[(size_t) block];
    bipolar = section != Amp;
    if (section == Pitch)  return &sb.pitchEg;
    if (section == Filter) return &sb.filterEg;
    if (section == Amp)    return &sb.ampEg;
    return nullptr;
}

void SoundEditor::refresh()
{
    const int ui = userIndex();
    const bool valid = ui >= 0 && ui < proc.getNumUsers();
    setEnabled (valid);
    if (! valid) return;

    auto u = proc.getUser (ui);   // kopija: ref() vraca referencu u nju
    int k = 0;
    for (auto& row : rows)
        for (auto& prm : row)
            if (k < knobs.size())
                knobs[k++]->setValue (prm.ref (u, block));

    bool bip = false;
    if (auto* e = currentEnv (u, bip))
        envView.setEnvelope (*e, bip, Pal::part[block]);

    filterType.setSelectedItemIndex (u.block[(size_t) block].filterType, juce::dontSendNotification);
    oscOnButton.setToggleState (u.block[(size_t) block].on, juce::dontSendNotification);
    oscOnButton.setColour (juce::TextButton::buttonOnColourId, Pal::part[block]);
    voiceMode.setSelectedItemIndex (u.voiceMode, juce::dontSendNotification);
    priority.setSelectedItemIndex (u.priority, juce::dontSendNotification);
    portaOn.setToggleState (u.portaOn, juce::dontSendNotification);
    portaFingered.setToggleState (u.portaFingered, juce::dontSendNotification);
    portaConstRate.setToggleState (u.portaConstRate, juce::dontSendNotification);
    portaTime->setValue (u.portamento);
    legatoXfade->setValue (u.legatoXfade);
    legatoStart->setValue (u.legatoStart);
    refreshLegatoKnobs();
    setSliderValue (bendUp, u.bendUp);
    setSliderValue (bendDown, u.bendDown);
    fxType.setSelectedItemIndex (u.fxType, juce::dontSendNotification);

    const auto& sb = u.block[(size_t) block];
    setSliderValue (monoLow, sb.monoLow);
    setSliderValue (monoHigh, sb.monoHigh);
    monoLow.updateText();
    monoHigh.updateText();
    articOn.setToggleState (sb.articOn, juce::dontSendNotification);
    articDir.setSelectedItemIndex (sb.articDir, juce::dontSendNotification);
    for (int i = 0; i < 24; ++i)
    {
        intervalButtons[i].setToggleState ((sb.articMask & (1 << i)) != 0, juce::dontSendNotification);
        intervalButtons[i].setEnabled (sb.articOn);
    }

    // OSC-ovi koji imaju MS su jasniji
    for (int b = 0; b < kNumBlocks; ++b)
    {
        bool any = false;
        for (int m = 0; m < kMsPerOsc; ++m)
            any = any || u.osc[(size_t) (b * kMsPerOsc + m)].instrument.isNotEmpty();
        blockButtons[b].setAlpha (any || b == block ? 1.0f : 0.45f);
        blockButtons[b].setButtonText ("OSC" + juce::String (b + 1) + (u.block[(size_t) b].on ? "" : " off"));
    }
}

void SoundEditor::showBlockMenu()
{
    const int ui = userIndex();
    if (ui < 0 || ui >= proc.getNumUsers()) return;
    juce::PopupMenu m;
    m.addItem (1, T ("Copy OSC") + juce::String (block + 1));
    m.addItem (2, T ("Paste into OSC") + juce::String (block + 1), Clip::osc.has_value());
    m.addSeparator();
    m.addItem (3, T ("Delete OSC") + juce::String (block + 1));
    juce::Component::SafePointer<SoundEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&blockMenu), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        const int b = safe->block;
        if (r == 1)
        {
            const auto& u = safe->proc.getUser (safe->userIndex());
            Clip::OscCopy c { u.block[(size_t) b], {} };
            for (int i = 0; i < kMsPerOsc; ++i) c.ms[(size_t) i] = u.osc[(size_t) (b * kMsPerOsc + i)];
            Clip::osc = c;
            return;
        }
        safe->edit ([r, b] (UserProgram& u)
        {
            u.block[(size_t) b] = (r == 2 && Clip::osc) ? Clip::osc->block : SoundBlock();
            for (int i = 0; i < kMsPerOsc; ++i)
            {
                auto& o = u.osc[(size_t) (b * kMsPerOsc + i)];
                o = (r == 2 && Clip::osc) ? Clip::osc->ms[(size_t) i] : OscSettings();
                o.block = b;
            }
        });
        if (safe->onBlockChanged) safe->onBlockChanged();
    });
}

void SoundEditor::paint (juce::Graphics& g)
{
    g.setColour (Pal::bg.withAlpha (0.6f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}

void SoundEditor::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    auto top = r.removeFromTop (28);
    for (auto& b : blockButtons)
        b.setBounds (top.removeFromLeft (50).reduced (1, 0));
    blockMenu.setBounds (top.removeFromLeft (20));
    top.removeFromLeft (4);
    oscOnButton.setBounds (top.removeFromLeft (40).reduced (1, 0));
    top.removeFromLeft (8);
    const int sw = juce::jmin (110, top.getWidth() / NumSections);
    for (auto& b : sectionButtons)
        b.setBounds (top.removeFromLeft (sw).reduced (2, 0));
    r.removeFromTop (6);

    if (section == MonoArtic)
    {
        auto row = r.removeFromTop (48);
        auto c1 = row.removeFromLeft (360);
        monoLabel.setBounds (c1.removeFromTop (18));
        monoLow.setBounds (c1.removeFromLeft (120).reduced (0, 2));
        c1.removeFromLeft (10);
        monoHigh.setBounds (c1.removeFromLeft (120).reduced (0, 2));
        for (auto* s : { &monoLow, &monoHigh })
            s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 24);
        r.removeFromTop (6);
        articLabel.setBounds (r.removeFromTop (18));
        auto row2 = r.removeFromTop (30);
        articOn.setBounds (row2.removeFromLeft (100).reduced (0, 2));
        row2.removeFromLeft (8);
        articDir.setBounds (row2.removeFromLeft (140).reduced (0, 2));
        r.removeFromTop (4);
        auto row3 = r.removeFromTop (28);
        const int bw = juce::jmin (40, row3.getWidth() / 24);
        for (auto& b : intervalButtons)
            b.setBounds (row3.removeFromLeft (bw).reduced (1, 0));
        r.removeFromTop (6);
        articHelp.setBounds (r.removeFromTop (54));
        return;
    }

    const bool isBlock = section == Pitch || section == Filter || section == Amp;
    if (isBlock)
        envView.setBounds (r.removeFromRight (230).reduced (0, 4));

    if (section == Common)
    {
        auto row = r.removeFromTop (90).reduced (0, 6);
        auto col = [&] (int w) { auto c = row.removeFromLeft (w); row.removeFromLeft (14); return c; };
        auto c1 = col (160);
        commonLabels[0].setBounds (c1.removeFromTop (18));
        voiceMode.setBounds (c1.removeFromTop (28));
        auto c2 = col (160);
        commonLabels[1].setBounds (c2.removeFromTop (18));
        priority.setBounds (c2.removeFromTop (28));
        auto c3 = col (190);
        portaOn.setBounds (c3.removeFromTop (30));
        c3.removeFromTop (6);
        portaFingered.setBounds (c3.removeFromTop (26));
        portaTime->setBounds (col (90));
        auto c4 = col (170);
        c4.removeFromTop (36);
        portaConstRate.setBounds (c4.removeFromTop (26));
        auto c5 = col (100);
        commonLabels[2].setBounds (c5.removeFromTop (18));
        bendUp.setBounds (c5.removeFromTop (26));
        auto c6 = col (100);
        commonLabels[3].setBounds (c6.removeFromTop (18));
        bendDown.setBounds (c6.removeFromTop (26));
        for (auto* s : { &bendUp, &bendDown })
            s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 26);
        auto row2 = r.removeFromTop (100).reduced (0, 4);
        legatoXfade->setBounds (row2.removeFromLeft (100));
        row2.removeFromLeft (8);
        legatoStart->setBounds (row2.removeFromLeft (100));
        return;
    }

    if (section == Effect)
    {
        auto row = r.removeFromTop (90);
        auto c = row.removeFromLeft (200).reduced (0, 10);
        fxLabel.setBounds (c.removeFromTop (18));
        fxType.setBounds (c.removeFromTop (28));
        row.removeFromLeft (16);
        const int kw = juce::jmin (90, row.getWidth() / juce::jmax (1, knobs.size()));
        for (auto* k : knobs)
            k->setBounds (row.removeFromLeft (kw));
        return;
    }

    if (section == Filter)
    {
        auto c = r.removeFromLeft (150).reduced (0, 10);
        filterLabel.setBounds (c.removeFromTop (18));
        filterType.setBounds (c.removeFromTop (28).withTrimmedRight (10));
    }

    const int rowH = juce::jmin (90, r.getHeight() / juce::jmax (1, (int) rows.size()));
    int k = 0;
    for (auto& row : rows)
    {
        auto line = r.removeFromTop (rowH);
        const int kw = juce::jmin (88, line.getWidth() / juce::jmax (1, (int) row.size()));
        for (size_t i = 0; i < row.size() && k < knobs.size(); ++i)
            knobs[k++]->setBounds (line.removeFromLeft (kw));
    }
}

//==============================================================================
//  FmEditor
//==============================================================================
FmEditor::FmEditor (LiveSamplerProcessor& p, std::function<int()> ui) : proc (p), userIndex (std::move (ui))
{
    algLabel.setText ("ALGORITHM", juce::dontSendNotification);
    algLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    algLabel.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (algLabel);
    algorithm.addItem (T ("Kronos patch (cables)"), 100);
    for (int n = 1; n <= 32; ++n) algorithm.addItem ("DX " + juce::String (n), n);
    algorithm.setTooltip (T ("DX7 algorithm: which operators modulate which (carriers are heard). 'Kronos patch' = the cables of the MOD-7 program."));
    algorithm.onChange = [this]
    {
        if (updating) return;
        const int id = algorithm.getSelectedId();
        if (id >= 1 && id <= 32) edit ([id] (Fm::Patch& pt) { pt.applyDxAlgorithm (id); });
        else refresh();   // kablovi se ne mogu napraviti iz menija
    };
    addAndMakeVisible (algorithm);

    UI::setupIncDec (transpose, -48, 48, 1, [] (double v) { return (v > 0 ? "+" : "") + juce::String ((int) v) + " st"; });
    transpose.setTooltip (T ("Transpose of the FM sound (semitones)"));
    transpose.onValueChange = [this] { if (! updating) { const int v = (int) transpose.getValue(); edit ([v] (Fm::Patch& pt) { pt.transpose = v; }); } };
    addAndMakeVisible (transpose);
    level = std::make_unique<ValueKnob> ("Level", Pal::accent, 0.0, 1.0, 0.5, UI::pctText, 1.0);
    level->onChange = [this] (double v) { if (! updating) edit ([v] (Fm::Patch& pt) { pt.ampLevel = (float) v; }); };
    velocity = std::make_unique<ValueKnob> ("Velocity", Pal::accent, -1.0, 1.0, 0.0, UI::pctText, 0.6);
    velocity->onChange = [this] (double v) { if (! updating) edit ([v] (Fm::Patch& pt) { pt.ampVel = (float) v; }); };
    addAndMakeVisible (*level);
    addAndMakeVisible (*velocity);

    const char* cols[] = { "OP", "WAVE", "RATIO", "FINE", "FIXED Hz", "LEVEL", "VEL", "FEEDBACK" };
    for (int i = 0; i < 8; ++i)
    {
        colLabels[i].setText (cols[i], juce::dontSendNotification);
        colLabels[i].setFont (juce::FontOptions (11.5f, juce::Font::bold));
        colLabels[i].setColour (juce::Label::textColourId, Pal::dim);
        colLabels[i].setJustificationType (juce::Justification::centred);
        addAndMakeVisible (colLabels[i]);
    }
    for (int r = 0; r < 6; ++r)
    {
        auto& o = ops[r];
        const int k = Fm::opIndex (r + 1);   // red r = DX operator r+1
        o.on.setButtonText ("OP" + juce::String (r + 1));
        o.on.setClickingTogglesState (true);
        o.on.setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        o.on.onClick = [this, k, r] { const bool b = ops[r].on.getToggleState(); edit ([k, b] (Fm::Patch& pt) { pt.op[k].on = b; }); };
        o.wave.addItemList ({ "Sine", "Triangle", "Square", "Saw", "Sine + shaper", "Shaper (input)", "Ring mod" }, 1);
        o.wave.onChange = [this, k, r] { if (! updating) { const int m = ops[r].wave.getSelectedId(); edit ([k, m] (Fm::Patch& pt) { pt.op[k].mode = m; }); } };
        UI::setupIncDec (o.coarse, 0, 32, 1, [] (double v) { return v < 0.5 ? juce::String ("0.5") : juce::String ((int) v); });
        o.coarse.setTooltip (T ("Frequency ratio (multiple of the note frequency), 0 = 0.5"));
        o.coarse.onValueChange = [this, k, r]
        {
            if (updating) return;
            const double c = ops[r].coarse.getValue();
            edit ([k, c] (Fm::Patch& pt)
            {
                const double old = pt.op[k].ratio;
                const double fine = old - (old < 1.0 ? 0.5 : std::floor (old));
                pt.op[k].ratio = (c < 0.5 ? 0.5 : c) + juce::jlimit (0.0, 0.999, fine);
            });
        };
        UI::setupIncDec (o.fine, 0, 0.99, 0.01, [] (double v) { return "+" + juce::String (v, 2); });
        o.fine.onValueChange = [this, k, r]
        {
            if (updating) return;
            const double f = ops[r].fine.getValue();
            edit ([k, f] (Fm::Patch& pt) { const double old = pt.op[k].ratio; pt.op[k].ratio = (old < 1.0 ? 0.5 : std::floor (old)) + f; });
        };
        UI::setupIncDec (o.fixedHz, 0, 9999, 1, [] (double v) { return v < 0.5 ? juce::String ("off") : juce::String ((int) v) + " Hz"; });
        o.fixedHz.setTooltip (T ("Fixed frequency added to the ratio (Hz). With ratio 0.5 and fine 0 it is almost a fixed pitch (bells, noise)."));
        o.fixedHz.onValueChange = [this, k, r] { if (! updating) { const double v = ops[r].fixedHz.getValue(); edit ([k, v] (Fm::Patch& pt) { pt.op[k].offsetHz = v; }); } };
        UI::setupIncDec (o.volume, 0, 100, 1, [] (double v) { return juce::String ((int) v) + "%"; });
        o.volume.setTooltip (T ("Operator level. For a carrier it is volume, for a modulator it is brightness (modulation depth)."));
        o.volume.onValueChange = [this, k, r] { if (! updating) { const float v = (float) ops[r].volume.getValue() / 100.0f; edit ([k, v] (Fm::Patch& pt) { pt.op[k].volume = v; }); } };
        UI::setupIncDec (o.vel, -100, 100, 1, [] (double v) { return juce::String ((int) v); });
        o.vel.setTooltip (T ("Velocity sensitivity of this operator"));
        o.vel.onValueChange = [this, k, r] { if (! updating) { const float v = (float) ops[r].vel.getValue() / 100.0f; edit ([k, v] (Fm::Patch& pt) { pt.op[k].velSens = v; }); } };
        UI::setupIncDec (o.feedback, 0, 100, 1, [] (double v) { return juce::String ((int) v); });
        o.feedback.setTooltip (T ("Self feedback (the operator modulates itself: brighter, saw-like, noisy at the top)"));
        o.feedback.onValueChange = [this, k, r] { if (! updating) { const float v = (float) ops[r].feedback.getValue() / 100.0f; edit ([k, v] (Fm::Patch& pt) { pt.op[k].feedback = v; }); } };
        o.egButton.setButtonText ("EG");
        o.egButton.setTooltip (T ("Edit the envelope of this operator below"));
        o.egButton.onClick = [this, r] { selectEg (r); };
        const juce::String lid = "user.fm.op" + juce::String (r + 1) + ".";
        o.volume.setLearnId (lid + "level");
        o.feedback.setLearnId (lid + "feedback");
        o.coarse.setLearnId (lid + "ratio");
        for (juce::Component* c : { (juce::Component*) &o.on, (juce::Component*) &o.wave, (juce::Component*) &o.coarse, (juce::Component*) &o.fine,
                                    (juce::Component*) &o.fixedHz, (juce::Component*) &o.volume, (juce::Component*) &o.vel, (juce::Component*) &o.feedback,
                                    (juce::Component*) &o.egButton })
            addAndMakeVisible (c);
    }
    ampEgButton.setButtonText ("AMP EG");
    ampEgButton.setTooltip (T ("Edit the amplitude envelope (volume of the whole sound)"));
    ampEgButton.onClick = [this] { selectEg (6); };
    addAndMakeVisible (ampEgButton);

    egTitle.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    egTitle.setColour (juce::Label::textColourId, Pal::accent);
    addAndMakeVisible (egTitle);
    struct K { const char* n; double lo, hi, c, def; bool time; };
    const K ks[] = { { "Start", -1, 1, 0, 0, false }, { "Attack lvl", -1, 1, 0, 1, false }, { "Break lvl", -1, 1, 0, 1, false },
                     { "Sustain", -1, 1, 0, 1, false }, { "Release lvl", -1, 1, 0, 0, false },
                     { "Attack", 0.001, 30, 1.0, 0.003, true }, { "Decay", 0.001, 30, 1.0, 0.3, true },
                     { "Slope", 0.001, 30, 1.0, 0.3, true }, { "Release", 0.001, 30, 1.0, 0.3, true } };
    for (int i = 0; i < 9; ++i)
    {
        auto* kn = egKnobs.add (new ValueKnob (T (ks[i].n), Pal::accent, ks[i].lo, ks[i].hi, ks[i].c,
                                               ks[i].time ? std::function<juce::String (double)> (UI::timeText) : std::function<juce::String (double)> ([] (double v) { return juce::String (juce::roundToInt (v * 99.0)); }),
                                               ks[i].def));
        kn->onChange = [this, i] (double v)
        {
            if (updating) return;
            const int which = egSel;
            edit ([which, i, v] (Fm::Patch& pt)
            {
                EnvSettings* e = nullptr;
                if (which == 6) e = &pt.ampEg;
                else { const int egi = pt.op[Fm::opIndex (which + 1)].eg; if (egi >= 1 && egi <= 9) e = &pt.eg[egi - 1]; else if (egi == 10) e = &pt.ampEg; }
                if (e == nullptr) return;
                float* f[] = { &e->start, &e->attackLevel, &e->breakLevel, &e->sustain, &e->releaseLevel, &e->attackTime, &e->decayTime, &e->slopeTime, &e->releaseTime };
                *f[i] = (float) v;
            });
        };
        addAndMakeVisible (kn);
    }
}

Fm::Patch FmEditor::current() const
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers() || ! proc.getUser (u).isFm()) return {};
    if (auto p = const_cast<LiveSamplerProcessor&> (proc).fmPatchFor (proc.getUser (u))) return *p;
    return {};
}

void FmEditor::edit (std::function<void (Fm::Patch&)> fn)
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    auto p = current();
    fn (p);
    auto user = proc.getUser (u);
    user.fmPatch = p.toString();
    user.fmProgram = {};            // od sada se cuvaju parametri (editovani)
    proc.updateUser (u, user);
    repaint();
}

void FmEditor::selectEg (int which)
{
    egSel = which;
    refresh();
}

void FmEditor::refresh()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    const auto p = current();
    algorithm.setSelectedId (p.dxAlgorithm >= 1 ? p.dxAlgorithm : 100, juce::dontSendNotification);
    transpose.setValue (p.transpose, juce::dontSendNotification);
    level->setValue (p.ampLevel);
    velocity->setValue (p.ampVel);
    const auto carriers = Fm::dxCarriers (p.dxAlgorithm);
    for (int r = 0; r < 6; ++r)
    {
        const auto& o = p.op[Fm::opIndex (r + 1)];
        auto& row = ops[r];
        row.on.setToggleState (o.on, juce::dontSendNotification);
        row.wave.setSelectedId (juce::jlimit (1, 7, o.mode), juce::dontSendNotification);
        const double base = o.ratio < 1.0 ? 0.0 : std::floor (o.ratio);
        row.coarse.setValue (base, juce::dontSendNotification);
        row.fine.setValue (o.ratio - (o.ratio < 1.0 ? 0.5 : base), juce::dontSendNotification);
        row.fixedHz.setValue (o.offsetHz, juce::dontSendNotification);
        row.volume.setValue (o.volume * 100.0, juce::dontSendNotification);
        row.vel.setValue (o.velSens * 100.0, juce::dontSendNotification);
        row.feedback.setValue (o.feedback * 100.0, juce::dontSendNotification);
        const bool carrier = std::find (carriers.begin(), carriers.end(), r + 1) != carriers.end();
        row.on.setColour (juce::TextButton::buttonOnColourId, carrier ? Pal::accent : juce::Colour (0xff3dc5ff));
        row.on.setTooltip (carrier ? T ("Carrier (heard)") : T ("Modulator (changes the tone of the operator below it)"));
        row.egButton.setToggleState (egSel == r, juce::dontSendNotification);
        row.egButton.setColour (juce::TextButton::buttonColourId, egSel == r ? Pal::accent.withAlpha (0.6f) : Pal::panelHi);
    }
    ampEgButton.setColour (juce::TextButton::buttonColourId, egSel == 6 ? Pal::accent.withAlpha (0.6f) : Pal::panelHi);

    const EnvSettings* e = nullptr;
    juce::String title;
    if (egSel == 6) { e = &p.ampEg; title = T ("AMP EG (volume of the whole sound)"); }
    else
    {
        const int egi = p.op[Fm::opIndex (egSel + 1)].eg;
        if (egi >= 1 && egi <= 9) { e = &p.eg[egi - 1]; title = "OP" + juce::String (egSel + 1) + " EG  (EG" + juce::String (egi) + ")"; }
        else if (egi == 10) { e = &p.ampEg; title = "OP" + juce::String (egSel + 1) + T (" uses the AMP EG"); }
        else title = "OP" + juce::String (egSel + 1) + T (": no envelope (constant level)");
    }
    egTitle.setText (title, juce::dontSendNotification);
    const float vals[] = { e ? e->start : 0, e ? e->attackLevel : 0, e ? e->breakLevel : 0, e ? e->sustain : 0, e ? e->releaseLevel : 0,
                           e ? e->attackTime : 0.001f, e ? e->decayTime : 0.001f, e ? e->slopeTime : 0.001f, e ? e->releaseTime : 0.001f };
    for (int i = 0; i < 9; ++i)
    {
        egKnobs[i]->setValue (vals[i]);
        egKnobs[i]->setEnabled (e != nullptr);
    }
    repaint();
}

void FmEditor::paint (juce::Graphics& g)
{
    g.setColour (Pal::bg);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    // sema algoritma: nosioci dole, modulatori iznad onoga koga moduliraju
    const auto p = current();
    auto area = diagram.toFloat();
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (area, 6.0f);
    if (p.dxAlgorithm < 1)
    {
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (T ("Kronos MOD-7 patch:\nconnections made with cables\non the Kronos (not editable here)"), diagram.reduced (8), juce::Justification::centred, 4);
        return;
    }
    const auto mods = Fm::dxModulations (p.dxAlgorithm);
    const auto carriers = Fm::dxCarriers (p.dxAlgorithm);
    int depth[7] {};
    std::function<void (int, int)> place = [&] (int op, int d)
    {
        if (depth[op] >= d && depth[op] > 0) return;
        depth[op] = d;
        for (auto& m : mods) if (m.first == op && m.second != op) place (m.second, d + 1);
    };
    for (int c : carriers) place (c, 1);
    int maxD = 1;
    for (int op = 1; op <= 6; ++op) { if (depth[op] == 0) depth[op] = 1; maxD = juce::jmax (maxD, depth[op]); }
    // x: redom po dubini
    juce::Point<float> pos[7];
    for (int d = 1; d <= maxD; ++d)
    {
        std::vector<int> row;
        for (int op = 1; op <= 6; ++op) if (depth[op] == d) row.push_back (op);
        for (size_t i = 0; i < row.size(); ++i)
            pos[row[i]] = { area.getX() + area.getWidth() * ((float) i + 0.5f) / (float) row.size(),
                            area.getBottom() - 18.0f - (float) (d - 1) * (area.getHeight() - 36.0f) / (float) juce::jmax (1, maxD - 1) * (maxD > 1 ? 1.0f : 0.0f) };
    }
    g.setColour (Pal::dim);
    for (auto& m : mods)
        g.drawLine (juce::Line<float> (pos[m.second], pos[m.first]), 1.6f);
    for (int op = 1; op <= 6; ++op)
    {
        const bool carrier = std::find (carriers.begin(), carriers.end(), op) != carriers.end();
        const bool on = p.op[Fm::opIndex (op)].on;
        auto r = juce::Rectangle<float> (26.0f, 20.0f).withCentre (pos[op]);
        g.setColour ((carrier ? Pal::accent : juce::Colour (0xff3dc5ff)).withAlpha (on ? 0.9f : 0.3f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (juce::String (op), r, juce::Justification::centred);
        if (p.op[Fm::opIndex (op)].feedback > 0.0f)
        {
            g.setColour (Pal::text);
            g.drawEllipse (r.getRight() - 2.0f, r.getY() - 6.0f, 8.0f, 8.0f, 1.2f);
        }
    }
}

void FmEditor::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    diagram = r.removeFromRight (180).withTrimmedBottom (r.getHeight() > 190 ? r.getHeight() - 190 : 0);
    r.removeFromRight (8);
    auto top = r.removeFromTop (26);
    algLabel.setBounds (top.removeFromLeft (76));
    algorithm.setBounds (top.removeFromLeft (190).reduced (0, 1));
    top.removeFromLeft (10);
    transpose.setBounds (top.removeFromLeft (120));
    transpose.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 64, top.getHeight());
    top.removeFromLeft (10);
    ampEgButton.setBounds (top.removeFromLeft (80).reduced (0, 1));
    r.removeFromTop (2);
    const int w[] = { 52, 104, 84, 88, 96, 84, 80, 80, 36 };
    {
        auto h = r.removeFromTop (14);
        for (int i = 0; i < 8; ++i) colLabels[i].setBounds (h.removeFromLeft (w[i]));
    }
    for (int row = 0; row < 6; ++row)
    {
        auto line = r.removeFromTop (24);
        r.removeFromTop (1);
        auto& o = ops[row];
        juce::Component* cs[] = { &o.on, &o.wave, &o.coarse, &o.fine, &o.fixedHz, &o.volume, &o.vel, &o.feedback, &o.egButton };
        for (int i = 0; i < 9; ++i)
        {
            auto cell = line.removeFromLeft (w[i]).reduced (1, 0);
            cs[i]->setBounds (cell);
            if (auto* s = dynamic_cast<juce::Slider*> (cs[i])) s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, cell.getWidth() - 34, cell.getHeight());
        }
    }
    r.removeFromTop (2);
    // donji red: envelope + jacina / velocity celog zvuka
    egTitle.setBounds (r.removeFromTop (16));
    auto knobs = r;
    level->setBounds (knobs.removeFromRight (74));
    velocity->setBounds (knobs.removeFromRight (74));
    const int kw = juce::jmin (78, knobs.getWidth() / 9);
    for (auto* k : egKnobs) k->setBounds (knobs.removeFromLeft (kw));
}

//==============================================================================
//  UserPage
//==============================================================================
UserPage::UserPage (LiveSamplerProcessor& p, LiveSamplerEditor& e)
    : proc (p), editor (e), fm (p, [this] { return userIndex; }), sound (p, [this] { return userIndex; })
{
    list.setModel (this);
    list.setRowHeight (26);
    list.setColour (juce::ListBox::outlineColourId, Pal::panelHi);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    newButton.setButtonText ("New");
    newButton.onClick = [this]
    {
        juce::Component::SafePointer<UserPage> safe (this);
        juce::PopupMenu m;
        m.addItem (1, T ("New user (samples)..."));
        m.addItem (2, T ("New FM user (FM synthesis, DX style)"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&newButton), [safe] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == 2) { safe->selectUser (safe->proc.createFmUser()); return; }
            UI::showInstrumentMenu (&safe->newButton, safe->proc.getInstrumentList(), {}, true, [safe] (const juce::String& inst)
            {
                if (safe == nullptr) return;
                const int u = inst.isEmpty() ? safe->proc.createUser ("User") : safe->proc.createUserFromInstrument (inst);
                safe->selectUser (u);
            });
        });
    };
    addChildComponent (fm);
    copyButton.setButtonText ("Copy");
    copyButton.onClick = [this]
    {
        if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
        auto u = proc.getUser (userIndex);
        selectUser (proc.createUser (u.name, &u));
    };
    deleteButton.setButtonText (T ("Delete"));
    deleteButton.onClick = [this]
    {
        if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
        juce::Component::SafePointer<UserPage> safe (this);
        const int idx = userIndex;
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("Delete user"),
            T ("Delete user \"") + proc.getUser (idx).name + T ("\"? Performance parts that use it will have no sound."),
            T ("Delete"), T ("Cancel"), nullptr,
            juce::ModalCallbackFunction::create ([safe, idx] (int result)
            {
                if (safe == nullptr || result == 0) return;
                safe->proc.deleteUser (idx);
                safe->selectUser (juce::jmax (0, idx - 1));
            }));
    };
    for (auto* b : { &newButton, &copyButton, &deleteButton })
        addAndMakeVisible (b);

    search.setTextToShowWhenEmpty ("Search users...", Pal::dim);
    search.setFont (juce::FontOptions (14.0f));
    search.onTextChange = [this] { rebuildVisible(); list.updateContent(); list.repaint(); };
    search.onEscapeKey = [this] { search.clear(); rebuildVisible(); list.updateContent(); list.repaint(); };
    addAndMakeVisible (search);

    catFilter.onChange = [this] { rebuildVisible(); list.updateContent(); list.repaint(); };
    catFilter.setTooltip ("Show only users of this category");
    addAndMakeVisible (catFilter);

    sortButton.setButtonText ("#");
    sortButton.setTooltip ("List order: by number (#) or alphabetical (A-Z)");
    sortButton.onClick = [this] { sortAlpha = ! sortAlpha; sortButton.setButtonText (sortAlpha ? "A-Z" : "#"); rebuildVisible(); list.updateContent(); list.repaint(); };
    catButton.setButtonText ("Category");
    catButton.setTooltip ("Set the category of this user, or add / rename / delete categories");
    catButton.onClick = [this] { categoryMenu(); };
    numButton.setButtonText ("No.");
    numButton.setTooltip ("Change the number (position) of this user");
    numButton.onClick = [this] { askNumber(); };
    upButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb2"));
    upButton.setTooltip ("Move up (number - 1)");
    upButton.onClick = [this] { if (userIndex > 0) { proc.moveUser (userIndex, userIndex - 1); selectUser (userIndex - 1); } };
    downButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbc"));
    downButton.setTooltip ("Move down (number + 1)");
    downButton.onClick = [this] { if (userIndex < proc.getNumUsers() - 1) { proc.moveUser (userIndex, userIndex + 1); selectUser (userIndex + 1); } };
    for (auto* b : { &sortButton, &catButton, &numButton, &upButton, &downButton })
    {
        b->getProperties().set ("fontSize", 12.0f);
        addAndMakeVisible (b);
    }

    nameEdit.setEditable (true);
    nameEdit.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    nameEdit.setColour (juce::Label::backgroundColourId, Pal::lcdBg);
    nameEdit.setColour (juce::Label::textColourId, Pal::lcdText);
    nameEdit.setTooltip (T ("Click to rename the user"));
    nameEdit.onTextChange = [this]
    {
        if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
        const auto oldName = proc.getUser (userIndex).name;
        const auto newName = nameEdit.getText().trim();
        if (newName.isEmpty() || newName == oldName) { nameEdit.setText (oldName, juce::dontSendNotification); return; }
        const auto used = proc.userUsage (oldName);
        if (used.isEmpty()) { edit ([newName] (UserProgram& u) { u.name = newName; }); return; }
        // koristi se u performansima: pitaj, pa preimenuj i tamo (inace bi delovi izgubili zvuk)
        juce::String list;
        for (int i = 0; i < juce::jmin (12, used.size()); ++i) list << "\n   " << used[i];
        if (used.size() > 12) list << "\n   ... " << (used.size() - 12) << T (" more");
        juce::Component::SafePointer<UserPage> safe (this);
        const int idx = userIndex;
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("Rename user"),
            "\"" + oldName + "\"  ->  \"" + newName + "\"\n\n" + T ("It is used in ") + juce::String (used.size()) + T (" performances:") + list
                + T ("\n\nRename it there too? (the parts keep this sound)"),
            T ("Rename everywhere"), T ("Cancel"), nullptr,
            juce::ModalCallbackFunction::create ([safe, idx, oldName, newName] (int ok)
            {
                if (safe == nullptr) return;
                if (ok == 0 || idx >= safe->proc.getNumUsers()) { safe->nameEdit.setText (oldName, juce::dontSendNotification); return; }
                auto u = safe->proc.getUser (idx);
                u.name = newName;
                safe->proc.updateUser (idx, u);   // preimenuje i u svim performansima
            }));
    };
    addAndMakeVisible (nameEdit);

    const char* headers[] = { "Multisample", "", "", "Level dB", "Pan", "Transp", "Fine c", "Low", "High", "Vel lo / hi" };
    for (int i = 0; i < 10; ++i)
    {
        styleLabel (headerLabels[i], T (headers[i]), 11.5f);
        addAndMakeVisible (headerLabels[i]);
    }

    for (int o = 0; o < kMsPerOsc; ++o)
        addAndMakeVisible (rows.add (new OscRow (p, e, o, [this] { return userIndex; }, [this] { return sound.getBlock(); })));
    sound.onBlockChanged = [this] { for (auto* r : rows) r->refresh(); };
    addAndMakeVisible (sound);
}

void SoundEditor::refreshLegatoKnobs()
{
    const int ui = userIndex();
    const bool xf = section == Common && ui >= 0 && ui < proc.getNumUsers() && proc.getUser (ui).voiceMode == VoiceMode::MonoLegatoXfade;
    legatoXfade->setVisible (xf);
    legatoStart->setVisible (xf);
}

void UserPage::edit (std::function<void (UserProgram&)> fn)
{
    if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
    auto u = proc.getUser (userIndex);
    fn (u);
    proc.updateUser (userIndex, u);
}

void UserPage::selectUser (int index)
{
    userIndex = juce::jlimit (0, juce::jmax (0, proc.getNumUsers() - 1), index);
    rebuildVisible();
    list.updateContent();
    const auto it = std::find (visible.begin(), visible.end(), userIndex);
    if (it != visible.end()) list.selectRow ((int) (it - visible.begin()), false, true);
    else list.deselectAllRows();
    refresh();
}

void UserPage::rebuildVisible()
{
    // kategorije u filteru
    {
        const auto cats = proc.getUserCategories();
        const auto cur = catFilter.getText();
        if (catFilter.getNumItems() != cats.size() + 2 || [&] { for (int i = 0; i < cats.size(); ++i) if (catFilter.getItemText (i + 2) != cats[i]) return true; return false; }())
        {
            catFilter.clear (juce::dontSendNotification);
            catFilter.addItem ("All categories", 1);
            catFilter.addItem ("No category", 2);
            for (int i = 0; i < cats.size(); ++i) catFilter.addItem (cats[i], i + 3);
            int id = 1;
            for (int i = 0; i < catFilter.getNumItems(); ++i) if (catFilter.getItemText (i) == cur) id = catFilter.getItemId (i);
            catFilter.setSelectedId (id, juce::dontSendNotification);
        }
        if (catFilter.getSelectedId() == 0) catFilter.setSelectedId (1, juce::dontSendNotification);
    }
    const int cf = catFilter.getSelectedId();
    const auto catName = cf >= 3 ? catFilter.getText() : juce::String();
    const auto q = search.getText().trim();
    visible.clear();
    for (int i = 0; i < proc.getNumUsers(); ++i)
    {
        const auto& u = proc.getUser (i);
        if (cf == 2 && u.category.isNotEmpty()) continue;
        if (cf >= 3 && u.category != catName) continue;
        if (q.isNotEmpty())
        {
            bool all = true;
            for (auto& w : juce::StringArray::fromTokens (q, " ", ""))
                if (w.isNotEmpty() && ! u.name.containsIgnoreCase (w) && ! u.category.containsIgnoreCase (w)
                    && juce::String (i + 1) != w) all = false;
            if (! all) continue;
        }
        visible.push_back (i);
    }
    if (sortAlpha)
        std::stable_sort (visible.begin(), visible.end(), [this] (int a, int b)
        {
            return proc.getUser (a).name.compareNatural (proc.getUser (b).name) < 0;
        });
}

int UserPage::getNumRows() { return (int) visible.size(); }

void UserPage::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) visible.size()) return;
    const int ui = visible[(size_t) row];
    if (ui < 0 || ui >= proc.getNumUsers()) return;
    const auto& u = proc.getUser (ui);
    if (selected) g.fillAll (Pal::accent.withAlpha (0.35f));
    g.setColour (Pal::dim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (juce::String (ui + 1).paddedLeft ('0', 3), 4, 0, 30, h, juce::Justification::centredLeft);
    int right = w - 4;
    if (u.category.isNotEmpty())
    {
        g.setFont (juce::FontOptions (11.0f));
        const int cw = juce::jmin (80, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (11.0f), u.category) + 8);
        g.setColour (Pal::lcdText.withAlpha (0.7f));
        g.drawText (u.category, right - cw, 0, cw, h, juce::Justification::centredRight, true);
        right -= cw + 4;
    }
    g.setColour (Pal::text);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (u.name, 36, 0, right - 36, h, juce::Justification::centredLeft, true);
}

void UserPage::selectedRowsChanged (int lastRowSelected)
{
    if (lastRowSelected >= 0 && lastRowSelected < (int) visible.size() && visible[(size_t) lastRowSelected] != userIndex)
    {
        userIndex = visible[(size_t) lastRowSelected];
        refresh();
    }
}

void UserPage::categoryMenu()
{
    if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
    const auto cats = proc.getUserCategories();
    const auto cur = proc.getUser (userIndex).category;
    juce::PopupMenu m;
    m.addSectionHeader ("Category of \"" + proc.getUser (userIndex).name + "\"");
    for (int i = 0; i < cats.size(); ++i) m.addItem (i + 1, cats[i], true, cats[i] == cur);
    m.addItem (1000, "(no category)", true, cur.isEmpty());
    m.addSeparator();
    m.addItem (2000, "New category...");
    juce::PopupMenu ren, del;
    for (int i = 0; i < cats.size(); ++i) { ren.addItem (3000 + i, cats[i]); del.addItem (4000 + i, cats[i]); }
    m.addSubMenu ("Rename category", ren, cats.size() > 0);
    m.addSubMenu ("Delete category", del, cats.size() > 0);
    juce::Component::SafePointer<UserPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&catButton), [safe, cats] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r <= cats.size() || r == 1000)
        {
            const auto c = r == 1000 ? juce::String() : cats[r - 1];
            safe->edit ([c] (UserProgram& u) { u.category = c; });
            return;
        }
        if (r >= 4000) { safe->proc.editUserCategory (cats[r - 4000], {}); return; }
        const auto old = r >= 3000 ? cats[r - 3000] : juce::String();
        auto* aw = new juce::AlertWindow (old.isEmpty() ? "New category" : "Rename category", {}, juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", old, "Name:");
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, old] (int ok)
        {
            const auto name = aw->getTextEditorContents ("name").trim();
            if (safe == nullptr || ok == 0 || name.isEmpty()) return;
            safe->proc.editUserCategory (old, name);
            if (old.isEmpty()) safe->edit ([name] (UserProgram& u) { u.category = name; });   // nova: odmah na ovog usera
        }), true);
    });
}

void UserPage::askNumber()
{
    if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
    auto* aw = new juce::AlertWindow ("User number", "Move \"" + proc.getUser (userIndex).name + "\" to number (1-"
                                      + juce::String (proc.getNumUsers()) + "):", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("n", juce::String (userIndex + 1));
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<UserPage> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int ok)
    {
        if (safe == nullptr || ok == 0) return;
        const int n = aw->getTextEditorContents ("n").getIntValue();
        if (n < 1) return;
        const int to = juce::jlimit (0, safe->proc.getNumUsers() - 1, n - 1);
        safe->proc.moveUser (safe->userIndex, to);
        safe->selectUser (to);
    }), true);
}

void UserPage::refresh()
{
    if (userIndex >= proc.getNumUsers())
        userIndex = juce::jmax (0, proc.getNumUsers() - 1);
    rebuildVisible();
    list.updateContent();
    list.repaint();
    {
        const auto it = std::find (visible.begin(), visible.end(), userIndex);
        const int row = it != visible.end() ? (int) (it - visible.begin()) : -1;
        if (row >= 0 && list.getSelectedRow() != row) list.selectRow (row, true, true);
    }
    numButton.setButtonText (userIndex < proc.getNumUsers() ? "No. " + juce::String (userIndex + 1) : juce::String ("No."));

    const bool valid = userIndex >= 0 && userIndex < proc.getNumUsers();
    const bool isFm = valid && proc.getUser (userIndex).isFm();
    for (auto* r : rows)
    {
        r->setVisible (! isFm);
        if (! isFm) r->refresh();
    }
    for (auto& l : headerLabels) l.setVisible (! isFm);
    if (fm.isVisible() != isFm) { fm.setVisible (isFm); resized(); }
    if (isFm) fm.refresh();
    sound.refresh();

    for (auto* c : std::initializer_list<juce::Component*> { &nameEdit, &copyButton, &deleteButton, &catButton, &numButton, &upButton, &downButton })
        c->setEnabled (valid);
    if (! valid)
    {
        nameEdit.setText (T ("No users: click New"), juce::dontSendNotification);
        return;
    }
    if (! nameEdit.isBeingEdited())
        nameEdit.setText (proc.getUser (userIndex).name, juce::dontSendNotification);
}

void UserPage::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    r.removeFromLeft (262);
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r.toFloat(), 8.0f);
}

void UserPage::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (250);
    r.removeFromLeft (12);
    search.setBounds (left.removeFromTop (28));
    left.removeFromTop (4);
    {
        auto row = left.removeFromTop (26);
        sortButton.setBounds (row.removeFromRight (44));
        row.removeFromRight (4);
        catFilter.setBounds (row);
    }
    left.removeFromTop (4);
    auto btns = left.removeFromBottom (30);
    const int bw = btns.getWidth() / 3;
    newButton.setBounds (btns.removeFromLeft (bw).reduced (2, 0));
    copyButton.setBounds (btns.removeFromLeft (bw).reduced (2, 0));
    deleteButton.setBounds (btns.reduced (2, 0));
    left.removeFromBottom (4);
    {
        auto row = left.removeFromBottom (26);
        catButton.setBounds (row.removeFromLeft (84).reduced (2, 0));
        numButton.setBounds (row.removeFromLeft (76).reduced (2, 0));
        upButton.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
        downButton.setBounds (row.reduced (2, 0));
    }
    left.removeFromBottom (6);
    list.setBounds (left);

    r.reduce (10, 8);
    nameEdit.setBounds (r.removeFromTop (30).removeFromLeft (420));
    r.removeFromTop (4);

    // FM user: umesto MS1-MS8 FM editor na istom mestu (malo visi: envelope ima knobove)
    const bool fmUser = userIndex >= 0 && userIndex < proc.getNumUsers() && proc.getUser (userIndex).isFm();
    if (fmUser)
    {
        fm.setBounds (r.removeFromTop (318));
        r.removeFromTop (6);
        sound.setBounds (r);
        return;
    }
    // zaglavlje kolona (prati raspored u OscRow)
    auto header = r.removeFromTop (16).reduced (4, 0);
    header.removeFromLeft (70);
    const int boxW = 82;
    const int fixed = boxW * 8 + 56 + 6 + 72;
    headerLabels[0].setBounds (header.removeFromLeft (header.getWidth() - fixed));
    header.removeFromLeft (4);
    headerLabels[2].setBounds (header.removeFromLeft (52));
    header.removeFromLeft (2 + 72);
    for (int i = 3; i < 9; ++i)
        headerLabels[i].setBounds (header.removeFromLeft (boxW));
    headerLabels[9].setBounds (header);

    for (auto* row : rows)
    {
        row->setBounds (r.removeFromTop (28));
        r.removeFromTop (1);
    }
    r.removeFromTop (6);
    sound.setBounds (r);
}

//==============================================================================
//  TercePage
//==============================================================================
static const char* degreeNames[] = { "1", "b2", "2", "m3", "3", "4", "#4", "5", "b6", "6", "m7", "7" };

TercePage::RuleRow::RuleRow (TercePage& o, int i) : owner (o), index (i)
{
    for (int k = 0; k < 12; ++k)
    {
        notes[k].setButtonText (degreeNames[k]);
        notes[k].setClickingTogglesState (true);
        notes[k].setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3ddc84));
        notes[k].getProperties().set ("fontSize", 12.0f);
        notes[k].onClick = [this, k]
        {
            const bool on = notes[k].getToggleState();
            owner.edit ([this, k, on] (TercaSettings& t)
            {
                if (index >= (int) t.rules.size()) return;
                auto& m = t.rules[(size_t) index].mask;
                m = on ? (m | (1 << k)) : (m & ~(1 << k));
                m |= 1;   // tonika je uvek deo akorda
            });
        };
        addAndMakeVisible (notes[k]);
    }
    notes[0].setTooltip (T ("Chord root (always on)"));

    scale.onChange = [this]
    {
        const int s = scale.getSelectedItemIndex();
        owner.edit ([this, s] (TercaSettings& t) { if (index < (int) t.rules.size()) t.rules[(size_t) index].scale = s; });
    };
    addAndMakeVisible (scale);

    UI::setupIncDec (shift, -11, 11, 1, semis);
    shift.setTooltip (T ("Key shift from the chord root. E.g. E7 with +5 gives A minor."));
    shift.onValueChange = [this]
    {
        const int v = (int) shift.getValue();
        owner.edit ([this, v] (TercaSettings& t) { if (index < (int) t.rules.size()) t.rules[(size_t) index].rootShift = v; });
    };
    addAndMakeVisible (shift);

    example.setFont (juce::FontOptions (12.5f));
    example.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (example);

    deleteButton.setButtonText ("X");
    deleteButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff5a2230));
    deleteButton.onClick = [this]
    {
        juce::Component::SafePointer<TercePage> safe (&owner);
        const int idx = index;
        juce::MessageManager::callAsync ([safe, idx]
        {
            if (safe == nullptr) return;
            safe->edit ([idx] (TercaSettings& t) { if (idx < (int) t.rules.size()) t.rules.erase (t.rules.begin() + idx); });
            safe->rebuildRules();
        });
    };
    addAndMakeVisible (deleteButton);
}

void TercePage::RuleRow::refresh()
{
    const auto& t = owner.proc.getTerca();
    if (index >= (int) t.rules.size()) return;
    const auto& r = t.rules[(size_t) index];
    for (int k = 0; k < 12; ++k)
        notes[k].setToggleState ((r.mask & (1 << k)) != 0, juce::dontSendNotification);

    scale.clear (juce::dontSendNotification);
    for (auto& sc : t.scales)
        scale.addItem (sc.name, scale.getNumItems() + 1);
    scale.setSelectedItemIndex (r.scale, juce::dontSendNotification);
    setSliderValue (shift, r.rootShift);

    // primer: akord od D
    juce::StringArray chord;
    for (int k = 0; k < 12; ++k)
        if (r.mask & (1 << k)) chord.add (Terca::pcName (2 + k));
    example.setText (T ("e.g. ") + chord.joinIntoString (" ") + "  ->  " + Terca::keyName (2 + r.rootShift, t, r.scale),
                     juce::dontSendNotification);
}

void TercePage::RuleRow::paint (juce::Graphics& g)
{
    g.setColour (index % 2 == 0 ? Pal::bg : Pal::bg.brighter (0.03f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

void TercePage::RuleRow::resized()
{
    auto r = getLocalBounds().reduced (4, 3);
    for (auto& n : notes)
        n.setBounds (r.removeFromLeft (34).reduced (1, 0));
    r.removeFromLeft (8);
    scale.setBounds (r.removeFromLeft (110));
    r.removeFromLeft (6);
    shift.setBounds (r.removeFromLeft (92));
    shift.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, r.getHeight());
    deleteButton.setBounds (r.removeFromRight (30));
    r.removeFromLeft (8);
    example.setBounds (r);
}

TercePage::TercePage (LiveSamplerProcessor& p)
    : proc (p), velocity (T ("Harmony velocity"), juce::Colour (0xff3ddc84), 0.1, 1.5, 0.0, UI::pctText, 0.8)
{
    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3ddc84));
    onButton.onClick = [this] { proc.setTercaOn (onButton.getToggleState()); };
    addAndMakeVisible (onButton);

    stateLabel.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    stateLabel.setColour (juce::Label::textColourId, Pal::lcdText);
    stateLabel.setColour (juce::Label::backgroundColourId, Pal::lcdBg);
    stateLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (stateLabel);

    for (int i = 0; i < 12; ++i)
        keyRoot.addItem (Terca::pcName (i), i + 1);
    keyRoot.onChange = [this] { proc.setTercaKey (keyRoot.getSelectedItemIndex(), proc.getTercaScale()); };
    keyScale.onChange = [this] { proc.setTercaKey (proc.getTercaRoot(), keyScale.getSelectedItemIndex()); };
    addAndMakeVisible (keyRoot);
    addAndMakeVisible (keyScale);

    learnButton.setButtonText (T ("LEARN button"));
    learnButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    learnButton.onClick = [this] { proc.startLearn (! proc.isLearning()); };
    addAndMakeVisible (learnButton);
    learnLabel.setFont (juce::FontOptions (13.5f));
    addAndMakeVisible (learnLabel);

    voiceMode.addItemList ({ "Third above", "Third below", "Above and below" }, 1);
    voiceMode.onChange = [this] { const int v = voiceMode.getSelectedItemIndex(); edit ([v] (TercaSettings& t) { t.voiceMode = v; }); };
    addAndMakeVisible (voiceMode);

    velocity.onChange = [this] (double v) { edit ([v] (TercaSettings& t) { t.velocity = (float) v; }); };
    addAndMakeVisible (velocity);

    topOnly.setButtonText (T ("Top note only (melody)"));
    topOnly.onClick = [this] { const bool b = topOnly.getToggleState(); edit ([b] (TercaSettings& t) { t.topNoteOnly = b; }); };
    addAndMakeVisible (topOnly);

    // skale
    scaleSelect.onChange = [this]
    {
        scaleIndex = juce::jmax (0, scaleSelect.getSelectedItemIndex());
        refresh();
    };
    addAndMakeVisible (scaleSelect);

    scaleName.setEditable (true);
    scaleName.setColour (juce::Label::backgroundColourId, Pal::bg);
    scaleName.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    scaleName.setTooltip (T ("Click to rename the scale"));
    scaleName.onTextChange = [this]
    {
        const auto n = scaleName.getText().trim();
        if (n.isEmpty()) return;
        edit ([this, n] (TercaSettings& t) { if (scaleIndex < (int) t.scales.size()) t.scales[(size_t) scaleIndex].name = n; });
    };
    addAndMakeVisible (scaleName);

    addScale.setButtonText ("New scale");
    addScale.onClick = [this]
    {
        edit ([this] (TercaSettings& t)
        {
            auto copy = t.scales.empty() ? TercaSettings::makeDefault().scales[0] : t.scales[(size_t) juce::jmin (scaleIndex, (int) t.scales.size() - 1)];
            copy.name = "User scale " + juce::String ((int) t.scales.size() + 1);
            t.scales.push_back (copy);
        });
        scaleIndex = (int) proc.getTerca().scales.size() - 1;
        refresh();
    };
    deleteScale.setButtonText (T ("Delete"));
    deleteScale.onClick = [this]
    {
        if (proc.getTerca().scales.size() <= 1) return;
        const int idx = scaleIndex;
        edit ([idx] (TercaSettings& t)
        {
            t.scales.erase (t.scales.begin() + idx);
            for (auto& r : t.rules)
                if (r.scale == idx) r.scale = 0;
                else if (r.scale > idx) --r.scale;
        });
        scaleIndex = 0;
        rebuildRules();
    };
    addAndMakeVisible (addScale);
    addAndMakeVisible (deleteScale);

    styleLabel (aboveLabel, "ABOVE", 12.0f, Pal::text);
    styleLabel (belowLabel, "BELOW", 12.0f, Pal::text);
    addAndMakeVisible (aboveLabel);
    addAndMakeVisible (belowLabel);

    for (int i = 0; i < 12; ++i)
    {
        stepNames[i].setJustificationType (juce::Justification::centred);
        stepNames[i].setFont (juce::FontOptions (12.5f, juce::Font::bold));
        addAndMakeVisible (stepNames[i]);

        for (auto* s : { &above[i], &below[i] })
        {
            UI::setupIncDec (*s, -12, 12, 1, semis);
            s->setTextBoxStyle (juce::Slider::TextBoxAbove, false, 60, 22);
            s->setIncDecButtonsMode (juce::Slider::incDecButtonsNotDraggable);
            addAndMakeVisible (s);
        }
        above[i].onValueChange = [this, i]
        {
            const int v = (int) above[i].getValue();
            edit ([this, i, v] (TercaSettings& t) { if (scaleIndex < (int) t.scales.size()) t.scales[(size_t) scaleIndex].above[(size_t) i] = v; });
        };
        below[i].onValueChange = [this, i]
        {
            const int v = (int) below[i].getValue();
            edit ([this, i, v] (TercaSettings& t) { if (scaleIndex < (int) t.scales.size()) t.scales[(size_t) scaleIndex].below[(size_t) i] = v; });
        };
    }

    // pravila
    addRule.setButtonText ("Add rule");
    addRule.onClick = [this]
    {
        edit ([] (TercaSettings& t) { t.rules.push_back ({ 1 | (1 << 4) | (1 << 7), 0, 0 }); });
        rebuildRules();
    };
    addAndMakeVisible (addRule);
    rulesHelp.setText (T ("Turn on the chord notes (from the root). When you hold the HARMONY button and play such a chord, the selected scale is used. "
                          "Rules are checked in order, the first one that matches wins."), juce::dontSendNotification);
    rulesHelp.setFont (juce::FontOptions (12.5f));
    rulesHelp.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (rulesHelp);

    rulesView.setViewedComponent (&rulesHolder, false);
    rulesView.setScrollBarsShown (true, false);
    addAndMakeVisible (rulesView);

    rebuildRules();
}

void TercePage::edit (std::function<void (TercaSettings&)> fn)
{
    auto t = proc.getTerca();
    fn (t);
    proc.updateTerca (t);
}

void TercePage::rebuildRules()
{
    ruleRows.clear();
    const int n = (int) proc.getTerca().rules.size();
    for (int i = 0; i < n; ++i)
        rulesHolder.addAndMakeVisible (ruleRows.add (new RuleRow (*this, i)));
    resized();
    refresh();
}

void TercePage::refresh()
{
    const auto& t = proc.getTerca();
    if ((int) ruleRows.size() != (int) t.rules.size())
    {
        rebuildRules();
        return;
    }

    const bool on = proc.isTercaOn();
    onButton.setToggleState (on, juce::dontSendNotification);
    onButton.setButtonText (on ? "HARMONY: ON" : "HARMONY: OFF");
    stateLabel.setText (proc.getTercaText(), juce::dontSendNotification);
    keyRoot.setSelectedItemIndex (proc.getTercaRoot(), juce::dontSendNotification);
    keyScale.clear (juce::dontSendNotification);
    for (auto& sc : t.scales)
        keyScale.addItem (sc.name, keyScale.getNumItems() + 1);
    keyScale.setSelectedItemIndex (proc.getTercaScale(), juce::dontSendNotification);

    learnButton.setToggleState (proc.isLearning(), juce::dontSendNotification);
    learnLabel.setText (proc.isLearning() ? T ("Press a button on the keyboard...") : T ("Button: ") + t.learn.describe(),
                        juce::dontSendNotification);
    voiceMode.setSelectedItemIndex (t.voiceMode, juce::dontSendNotification);
    velocity.setValue (t.velocity);
    topOnly.setToggleState (t.topNoteOnly, juce::dontSendNotification);

    scaleIndex = juce::jlimit (0, juce::jmax (0, (int) t.scales.size() - 1), scaleIndex);
    scaleSelect.clear (juce::dontSendNotification);
    for (auto& sc : t.scales)
        scaleSelect.addItem (sc.name, scaleSelect.getNumItems() + 1);
    scaleSelect.setSelectedItemIndex (scaleIndex, juce::dontSendNotification);
    deleteScale.setEnabled (t.scales.size() > 1);

    if (scaleIndex < (int) t.scales.size())
    {
        const auto& sc = t.scales[(size_t) scaleIndex];
        if (! scaleName.isBeingEdited())
            scaleName.setText (sc.name, juce::dontSendNotification);
        // tonovi prikazani u trenutnom tonalitetu
        const int root = proc.getTercaRoot();
        for (int i = 0; i < 12; ++i)
        {
            stepNames[i].setText (juce::String (degreeNames[i]) + "\n" + Terca::pcName (root + i), juce::dontSendNotification);
            setSliderValue (above[i], sc.above[(size_t) i]);
            setSliderValue (below[i], sc.below[(size_t) i]);
            stepNames[i].setColour (juce::Label::textColourId, (sc.above[(size_t) i] == 0 && sc.below[(size_t) i] == 0) ? Pal::dim : Pal::text);
        }
    }

    for (auto* r : ruleRows)
        r->refresh();
}

void TercePage::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (330);
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (left.toFloat(), 8.0f);
    r.removeFromLeft (12);
    auto scalesArea = r.removeFromTop (190);
    g.fillRoundedRectangle (scalesArea.toFloat(), 8.0f);
    r.removeFromTop (10);
    g.fillRoundedRectangle (r.toFloat(), 8.0f);

    UI::sectionTitle (g, left.reduced (12, 0).withHeight (26).withY (left.getY() + 4), "HARMONY", juce::Colour (0xff3ddc84));
    UI::sectionTitle (g, scalesArea.reduced (12, 0).withHeight (26).withY (scalesArea.getY() + 4),
                      T ("SCALES: for each note how many semitones up / down (0 = no harmony)"), Pal::lcdText);
    UI::sectionTitle (g, r.reduced (12, 0).withHeight (26).withY (r.getY() + 4),
                      T ("RULES: which chord (or two notes) selects which scale"), Pal::lcdText);
}

void TercePage::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (330).reduced (12, 8);
    left.removeFromTop (30);
    onButton.setBounds (left.removeFromTop (40));
    left.removeFromTop (8);
    stateLabel.setBounds (left.removeFromTop (54));
    left.removeFromTop (8);
    auto keyRow = left.removeFromTop (28);
    keyRoot.setBounds (keyRow.removeFromLeft (80));
    keyRow.removeFromLeft (6);
    keyScale.setBounds (keyRow);
    left.removeFromTop (16);
    learnButton.setBounds (left.removeFromTop (34));
    left.removeFromTop (4);
    learnLabel.setBounds (left.removeFromTop (22));
    left.removeFromTop (14);
    voiceMode.setBounds (left.removeFromTop (28));
    left.removeFromTop (8);
    topOnly.setBounds (left.removeFromTop (26));
    left.removeFromTop (8);
    velocity.setBounds (left.removeFromTop (80).removeFromLeft (100));

    r.removeFromLeft (12);
    auto scales = r.removeFromTop (190).reduced (12, 8);
    scales.removeFromTop (30);
    auto sRow = scales.removeFromTop (28);
    scaleSelect.setBounds (sRow.removeFromLeft (180));
    sRow.removeFromLeft (8);
    scaleName.setBounds (sRow.removeFromLeft (200));
    sRow.removeFromLeft (8);
    addScale.setBounds (sRow.removeFromLeft (110));
    sRow.removeFromLeft (6);
    deleteScale.setBounds (sRow.removeFromLeft (80));
    scales.removeFromTop (6);

    auto labelsCol = scales.removeFromLeft (56);
    const int colW = scales.getWidth() / 12;
    auto names = scales.removeFromTop (30);
    labelsCol.removeFromTop (30);
    aboveLabel.setBounds (labelsCol.removeFromTop (44));
    belowLabel.setBounds (labelsCol.removeFromTop (44));
    for (int i = 0; i < 12; ++i)
    {
        stepNames[i].setBounds (names.getX() + i * colW, names.getY(), colW, names.getHeight());
        above[i].setBounds (scales.getX() + i * colW + 2, scales.getY(), colW - 4, 42);
        below[i].setBounds (scales.getX() + i * colW + 2, scales.getY() + 44, colW - 4, 42);
    }

    r.removeFromTop (10);
    auto rules = r.reduced (12, 8);
    rules.removeFromTop (30);
    auto top = rules.removeFromTop (34);
    addRule.setBounds (top.removeFromLeft (130).reduced (0, 2));
    top.removeFromLeft (10);
    rulesHelp.setBounds (top);
    rules.removeFromTop (4);
    rulesView.setBounds (rules);

    const int rowH = 32;
    rulesHolder.setSize (juce::jmax (100, rules.getWidth() - 10), juce::jmax (rowH, (int) ruleRows.size() * rowH));
    for (int i = 0; i < ruleRows.size(); ++i)
        ruleRows[i]->setBounds (0, i * rowH, rulesHolder.getWidth(), rowH - 2);
}

//==============================================================================
//  EffectsPage
//==============================================================================
EffectsPage::EffectsPage (LiveSamplerProcessor& p) : proc (p)
{
    struct K { const char* name; double lo, hi, centre, def; std::function<juce::String (double)> fmt; };
    auto add = [this] (const char* title, std::initializer_list<K> ks)
    {
        sections.add ({ T (title), knobs.size(), (int) ks.size() });
        for (auto& k : ks)
            addAndMakeVisible (knobs.add (new ValueKnob (T (k.name), Pal::master, k.lo, k.hi, k.centre, k.fmt, k.def)));
    };
    auto db = [] (double v) { return juce::String (v, 1) + " dB"; };

    auto ms = [] (double v) { return juce::String (juce::roundToInt (v)) + " ms"; };
    auto sec = [] (double v) { return juce::String (v, 1) + " s"; };
    add ("PLATE REVERB", { { "Size", 0, 1, 0, 0.5, UI::pctText }, { "Decay", 0.3, 10, 2, 2.2, sec },
                           { "Predelay", 0, 150, 30, 20, ms }, { "Damping", 0, 1, 0, 0.5, UI::pctText },
                           { "Low cut", 20, 1000, 150, 150, UI::hzText }, { "High cut", 1000, 16000, 5000, 6000, UI::hzText },
                           { "Density", 0, 1, 0, 0.7, UI::pctText }, { "Modulation", 0, 1, 0, 0.3, UI::pctText },
                           { "Width", 0, 1, 0, 1.0, UI::pctText }, { "Level", 0, 1, 0, 0.8, UI::pctText } });
    add ("DELAY", { { "Time", 20, 1500, 350, 350, [] (double v) { return juce::String (juce::roundToInt (v)) + " ms"; } },
                    { "Feedback", 0, 0.95, 0, 0.35, UI::pctText }, { "Level", 0, 1, 0, 0.6, UI::pctText } });
    add ("EQ", { { "Low", -12, 12, 0, 0, db }, { "Mid", -12, 12, 0, 0, db }, { "High", -12, 12, 0, 0, db } });
    add ("PERFORMANCE", { { "Volume", -60, 6, -12, 0, UI::dbText } });

    auto setter = [this] (int i, std::function<void (MasterSettings&, float)> fn)
    {
        knobs[i]->onChange = [this, fn] (double v) { edit ([fn, v] (MasterSettings& m) { fn (m, (float) v); }); };
    };
    int k = 0;
    setter (k++, [] (MasterSettings& m, float v) { m.reverbSize = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbDecay = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbPreDelayMs = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbDamping = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbLowCut = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbHighCut = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbDiffusion = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbMod = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbWidth = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.reverbLevel = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.delayTimeMs = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.delayFeedback = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.delayLevel = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.eqLowDb = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.eqMidDb = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.eqHighDb = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.masterDb = v; });

    globalVolume = std::make_unique<ValueKnob> ("MASTER", juce::Colours::white, -60.0, 6.0, -12.0, UI::dbText, 0.0);
    globalVolume->onChange = [this] (double v) { proc.setGlobalVolume ((float) v); };
    addAndMakeVisible (*globalVolume);

    // MIDI Learn
    const char* ids[] = { "reverb.size", "reverb.decay", "reverb.predelay", "reverb.damping", "reverb.lowcut", "reverb.highcut",
                          "reverb.density", "reverb.mod", "reverb.width", "reverb.level", "delay.time", "delay.feedback", "delay.level",
                          "eq.low", "eq.mid", "eq.high", "perf.volume" };
    for (int i = 0; i < knobs.size() && i < 17; ++i)
        MidiLearn::get().bindSlider (knobs[i]->slider, juce::String ("fx.") + ids[i]);
    MidiLearn::get().bindSlider (globalVolume->slider, "master.volume");
}

void EffectsPage::edit (std::function<void (MasterSettings&)> fn)
{
    auto perf = proc.getPerformance();
    fn (perf.master);
    proc.updatePerformance (perf);
}

void EffectsPage::refresh()
{
    const auto& m = proc.getPerformance().master;
    const float vals[] = { m.reverbSize, m.reverbDecay, m.reverbPreDelayMs, m.reverbDamping, m.reverbLowCut, m.reverbHighCut,
                           m.reverbDiffusion, m.reverbMod, m.reverbWidth, m.reverbLevel, m.delayTimeMs, m.delayFeedback, m.delayLevel,
                           m.eqLowDb, m.eqMidDb, m.eqHighDb, m.masterDb };
    for (int i = 0; i < knobs.size(); ++i)
        knobs[i]->setValue (vals[i]);
    globalVolume->setValue (proc.getGlobalVolume());
}

void EffectsPage::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    for (auto& s : sections)
    {
        auto a = knobs[s.first]->getBounds();
        auto b = knobs[s.first + s.count - 1]->getBounds();
        UI::sectionTitle (g, juce::Rectangle<int> (a.getX(), a.getY() - 34, b.getRight() - a.getX(), 24), s.title, Pal::master);
    }
    auto gv = globalVolume->getBounds();
    UI::sectionTitle (g, juce::Rectangle<int> (gv.getX(), gv.getY() - 34, gv.getWidth(), 24), T ("MASTER (all performances)"), juce::Colours::white);
    g.setColour (Pal::dim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (T ("Effects are saved in the performance. How much each part sends to reverb and delay is set on the PERFORMANCE page."),
                getLocalBounds().reduced (20).removeFromBottom (24), juce::Justification::centredLeft);
}

void EffectsPage::resized()
{
    auto r = getLocalBounds().reduced (24, 16);
    r.removeFromBottom (28);
    const int rowH = r.getHeight() / 3;
    const int kw = juce::jmin (110, r.getWidth() / 10), kh = juce::jmin (110, rowH - 40), gap = 30;
    // red 1: reverb, red 2: delay + EQ, red 3: kompresor + performans + master
    const int rowOf[] = { 0, 1, 1, 2 };   // red 3: performans + master
    int x = 0, lastRow = -1, y = 0;
    for (int si = 0; si < sections.size(); ++si)
    {
        const int row = rowOf[juce::jmin (si, 3)];
        if (row != lastRow) { x = r.getX(); y = r.getY() + row * rowH + 36; lastRow = row; }
        auto& s = sections.getReference (si);
        for (int k = 0; k < s.count; ++k, x += kw)
            knobs[s.first + k]->setBounds (x, y, kw, kh);
        x += gap;
    }
    globalVolume->setBounds (x + 60, y, 160, kh);
}

//==============================================================================
//  SettingsPage
//==============================================================================
SettingsPage::SettingsPage (LiveSamplerProcessor& p) : proc (p)
{
    folders.setModel (this);
    folders.setRowHeight (30);
    folders.setColour (juce::ListBox::outlineColourId, Pal::panelHi);
    folders.setOutlineThickness (1);
    addAndMakeVisible (folders);

    addButton.setButtonText ("Add folder / USB");
    addButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (T ("Choose a sample folder (a whole USB stick works too)"),
                                                       juce::File::getSpecialLocation (juce::File::userHomeDirectory), "*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.isDirectory())
                    proc.addLibraryFolder (f);
            });
    };
    removeButton.setButtonText ("Remove");
    removeButton.onClick = [this] { proc.removeLibraryFolder (folders.getSelectedRow()); };
    rescanButton.setButtonText (T ("Refresh (USB inserted)"));
    rescanButton.onClick = [this] { proc.rescanLibrary(); };
    for (auto* b : { &addButton, &removeButton, &rescanButton })
        addAndMakeVisible (b);

    backupButton.setButtonText (T ("BACKUP WHOLE PROJECT..."));
    backupButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f7d4a));
    backupButton.setTooltip (T ("Users, performances, lists, set lists, harmony, categories and ALL samples used by the users, into one folder"));
    backupButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (T ("Backup folder (e.g. USB disk)"),
                                                       juce::File::getSpecialLocation (juce::File::userDesktopDirectory), "*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f == juce::File()) return;
                const auto dest = f.getChildFile ("Pikaso backup " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M"));
                if (! proc.startBackup (dest))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Backup", T ("Cannot create the folder ") + dest.getFullPathName());
                startTimerHz (4);
            });
    };
    restoreButton.setButtonText (T ("RESTORE BACKUP..."));
    restoreButton.setTooltip (T ("On another computer: choose the folder \"Pikaso backup ...\""));
    restoreButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (T ("Choose the folder \"Pikaso backup ...\""),
                                                       juce::File::getSpecialLocation (juce::File::userDesktopDirectory), "*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto folder = fc.getResult();
                if (! folder.isDirectory()) return;
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("Restore backup"),
                    T ("Users, performances, lists and other settings of this computer will be replaced with the ones from the backup:\n")
                        + folder.getFullPathName() + T ("\n\nThe current data is kept next to it, as a copy."),
                    T ("Restore"), T ("Cancel"), nullptr,
                    juce::ModalCallbackFunction::create ([this, folder] (int ok)
                    {
                        if (ok == 0) return;
                        juce::String rep;
                        const bool done = proc.restoreBackup (folder, rep);
                        juce::AlertWindow::showMessageBoxAsync (done ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon, T ("Restore backup"), rep);
                    }));
            });
    };
    for (auto* b : { &backupButton, &restoreButton }) addAndMakeVisible (b);
    backupStatus.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    backupStatus.setColour (juce::Label::textColourId, Pal::lcdText);
    addAndMakeVisible (backupStatus);

    velCurveLabel.setText (T ("Input velocity curve:"), juce::dontSendNotification);
    velCurveLabel.setColour (juce::Label::textColourId, Pal::text);
    addAndMakeVisible (velCurveLabel);
    {
        const char* names[] = { "Normal (no change)", "Lighter 1", "Lighter 2", "Lighter 3", "Lighter 4", "Heavier 1", "Heavier 2" };
        for (int i = 0; i < 7; ++i) velCurveBox.addItem (T (names[i]), i + 1);
    }
    velCurveBox.setTooltip (T ("If you have to play harder on the Impulse (or another keyboard) than on the Kronos, choose Lighter 1-4"));
    velCurveBox.onChange = [this] { proc.setVelocityCurve (velCurveBox.getSelectedId() - 1); };
    addAndMakeVisible (velCurveBox);

    polyLabel.setText (T ("Max voices per part"), juce::dontSendNotification);
    addAndMakeVisible (polyLabel);
    for (int v : { 16, 24, 32, 48, 64, 96, 128 }) polyBox.addItem (juce::String (v), v);
    polyBox.setTooltip (T ("When a part plays more voices, the oldest (released first) is stolen. Lower = less CPU on the mini PC"));
    polyBox.onChange = [this] { proc.setMaxPolyphony (polyBox.getSelectedId()); };
    addAndMakeVisible (polyBox);
    gigToggle.setButtonText (T ("GIG MODE: screen never sleeps, high priority (also on the LIVE page)"));
    gigToggle.onClick = [this] { proc.setGigMode (gigToggle.getToggleState()); };
    addAndMakeVisible (gigToggle);
    resetDrops.setButtonText (T ("Reset dropouts"));
    resetDrops.onClick = [this] { proc.resetDropouts(); refresh(); };
    addAndMakeVisible (resetDrops);
    audioLabel.setFont (juce::FontOptions (13.5f));
    audioLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (audioLabel);

    c3Toggle.setButtonText (T ("C3 = MIDI 60 (Yamaha/Korg). Off: C4 = 60"));
    c3Toggle.onClick = [this] { proc.setC3Is60 (c3Toggle.getToggleState()); };
    addAndMakeVisible (c3Toggle);

    info.setFont (juce::FontOptions (13.5f));
    info.setColour (juce::Label::textColourId, Pal::dim);
    info.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (info);
}

int SettingsPage::getNumRows() { return proc.getLibraryFolders().size(); }

void SettingsPage::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    auto f = proc.getLibraryFolders()[row];
    if (selected) g.fillAll (Pal::accent.withAlpha (0.35f));
    g.setColour (f.isDirectory() ? Pal::text : Pal::red);
    g.setFont (juce::FontOptions (14.5f));
    g.drawText (f.getFullPathName() + (f.isDirectory() ? juce::String() : T ("   (not available)")), 10, 0, w - 12, h,
                juce::Justification::centredLeft, true);
}

void SettingsPage::refresh()
{
    folders.updateContent();
    folders.repaint();
    c3Toggle.setToggleState (proc.getC3Is60(), juce::dontSendNotification);
    velCurveBox.setSelectedId (proc.getVelocityCurve() + 1, juce::dontSendNotification);
    polyBox.setSelectedId (proc.getMaxPolyphony(), juce::dontSendNotification);
    if (polyBox.getSelectedId() == 0) polyBox.setText (juce::String (proc.getMaxPolyphony()), juce::dontSendNotification);
    gigToggle.setToggleState (proc.isGigMode(), juce::dontSendNotification);
    {
        const auto a = proc.getAudioInfo();
        audioLabel.setText (T ("Audio: ") + (a.driver.isNotEmpty() ? a.driver : juce::String ("-")) + "  " + a.device + "   |   "
                            + juce::String (a.buffer) + T (" samples @ ") + juce::String (a.rate / 1000.0, 1) + " kHz   |   "
                            + T ("latency ") + juce::String (a.latencyMs, 1) + " ms   |   CPU " + juce::String (juce::roundToInt (proc.getAudioLoad() * 100.0f)) + "%   |   "
                            + juce::String (proc.getDropouts()) + T (" dropouts")
                            + (a.advice.isNotEmpty() ? "\n" + a.advice : T ("\nAudio OK")), juce::dontSendNotification);
        audioLabel.setColour (juce::Label::textColourId, a.warn ? juce::Colour (0xffffb340) : Pal::dim);
    }
    info.setText (T ("Each folder is scanned with its subfolders: WAV folders, Korg KMP and Kontakt NKI become instruments.\n"
                     "USB stick: insert it, click \"Add folder / USB\" and choose the stick. If you remove it and put it back, click \"Refresh\".\n"
                     "Audio interface and MIDI keyboard: the Options button in the top left corner of the window.\n\n"
                     "Users, performances and harmony are saved automatically in:\n") + proc.getDataFile().getFullPathName(),
                  juce::dontSendNotification);
}

void SettingsPage::timerCallback()
{
    backupStatus.setText (proc.getBackupStatus(), juce::dontSendNotification);
    backupButton.setEnabled (! proc.isBackupRunning());
    if (! proc.isBackupRunning()) stopTimer();
}

void SettingsPage::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    UI::sectionTitle (g, getLocalBounds().reduced (20, 0).withHeight (26).withY (8), "SAMPLE FOLDERS", Pal::lcdText);
}

void SettingsPage::resized()
{
    auto r = getLocalBounds().reduced (20, 10);
    r.removeFromTop (30);
    auto list = r.removeFromTop (200);
    auto btns = list.removeFromRight (200);
    folders.setBounds (list);
    btns.removeFromLeft (10);
    addButton.setBounds (btns.removeFromTop (34));
    btns.removeFromTop (6);
    removeButton.setBounds (btns.removeFromTop (34));
    btns.removeFromTop (6);
    rescanButton.setBounds (btns.removeFromTop (34));
    r.removeFromTop (16);
    {
        auto row = r.removeFromTop (34);
        backupButton.setBounds (row.removeFromLeft (240));
        row.removeFromLeft (8);
        restoreButton.setBounds (row.removeFromLeft (170));
        row.removeFromLeft (12);
        backupStatus.setBounds (row);
    }
    r.removeFromTop (12);
    c3Toggle.setBounds (r.removeFromTop (28));
    r.removeFromTop (6);
    {
        auto row = r.removeFromTop (28);
        velCurveLabel.setBounds (row.removeFromLeft (220));
        velCurveBox.setBounds (row.removeFromLeft (240).reduced (0, 1));
        row.removeFromLeft (30);
        polyLabel.setBounds (row.removeFromLeft (170));
        polyBox.setBounds (row.removeFromLeft (90).reduced (0, 1));
    }
    r.removeFromTop (6);
    gigToggle.setBounds (r.removeFromTop (28));
    r.removeFromTop (6);
    {
        auto row = r.removeFromTop (44);
        resetDrops.setBounds (row.removeFromRight (140).withHeight (28));
        audioLabel.setBounds (row);
    }
    r.removeFromTop (12);
    info.setBounds (r);
}

//==============================================================================
//  KronosPage
//==============================================================================
enum KronosCols { kcBank = 1, kcNum, kcName, kcOsc1, kcOsc2, kcState };

KronosPage::KronosPage (LiveSamplerProcessor& p, LiveSamplerEditor& e) : proc (p), editor (e)
{
    loadButton.setButtonText (T ("Load Kronos PCG..."));
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (T ("Choose a Kronos PCG file"),
                                                       proc.getKronosFile().existsAsFile() ? proc.getKronosFile() : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                       "*.PCG;*.pcg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) loadPcg (fc.getResult()); });
    };
    addAndMakeVisible (loadButton);

    fileLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    fileLabel.setColour (juce::Label::textColourId, Pal::lcdText);
    addAndMakeVisible (fileLabel);
    infoLabel.setFont (juce::FontOptions (13.0f));
    infoLabel.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (infoLabel);

    onlyUser.setButtonText (T ("Only programs with my samples"));
    onlyUser.setToggleState (true, juce::dontSendNotification);
    onlyUser.onClick = [this] { rebuildRows(); };
    addAndMakeVisible (onlyUser);

    keepChannels.setButtonText (T ("Keep MIDI channels from the combi"));
    keepChannels.setTooltip (T ("Off: all parts play from your keyboard, on any channel. Turn on only if you play from several keyboards on different channels."));
    addChildComponent (keepChannels);
    fmToggle.setButtonText (T ("MOD-7 as FM synth"));
    fmToggle.setTooltip (T ("On: MOD-7 programs (DX7 / V50 imports...) play as FM synthesis, live, without samples, even if they are resampled. "
                            "Off: the resampled sound is used, FM only for MOD-7 programs that are not resampled. "
                            "MOD-7 programs with a PCM oscillator or a complex patch are close to the Kronos, not identical."));
    fmToggle.setToggleState (proc.preferFm, juce::dontSendNotification);
    fmToggle.onClick = [this] { proc.preferFm = fmToggle.getToggleState(); };
    addAndMakeVisible (fmToggle);

    search.setTextToShowWhenEmpty (T ("Search by name..."), Pal::dim);
    search.onTextChange = [this] { rebuildRows(); };
    addAndMakeVisible (search);

    table.setModel (this);
    table.setRowHeight (24);
    table.setMultipleSelectionEnabled (true);
    table.setColour (juce::ListBox::backgroundColourId, Pal::bg);
    auto& h = table.getHeader();
    h.addColumn ("Bank", kcBank, 80, 60, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("No.", kcNum, 50, 40, 80, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Program", kcName, 230, 120, 400, juce::TableHeaderComponent::notSortable);
    h.addColumn ("OSC1 multisample", kcOsc1, 260, 120, 500, juce::TableHeaderComponent::notSortable);
    h.addColumn ("OSC2 multisample", kcOsc2, 260, 120, 500, juce::TableHeaderComponent::notSortable);
    h.addColumn ("User", kcState, 150, 80, 300, juce::TableHeaderComponent::notSortable);
    h.setColour (juce::TableHeaderComponent::backgroundColourId, Pal::panelHi);
    h.setColour (juce::TableHeaderComponent::textColourId, Pal::text);
    addAndMakeVisible (table);

    createSelected.setButtonText (T ("Create users from selected"));
    createSelected.onClick = [this]
    {
        auto* pcg = proc.getKronosPcg();
        if (pcg == nullptr) return;
        const auto sel = table.getSelectedRows();
        for (int i = 0; i < sel.size(); ++i)
            if (sel[i] >= 0 && sel[i] < (int) rows.size())
                userFor (pcg->programs[(size_t) rows[(size_t) sel[i]]]);
        table.repaint();
    };
    createAll.setButtonText (T ("Create users from all shown"));
    createAll.onClick = [this]
    {
        juce::Component::SafePointer<KronosPage> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Kronos",
            T ("Create users from ") + juce::String ((int) rows.size()) + T (" programs? Existing users with the same name are not duplicated."),
            T ("Create"), T ("Cancel"), nullptr,
            juce::ModalCallbackFunction::create ([safe] (int r)
            {
                if (safe == nullptr || r == 0 || safe->proc.getKronosPcg() == nullptr) return;
                for (int idx : safe->rows)
                    safe->userFor (safe->proc.getKronosPcg()->programs[(size_t) idx]);
                safe->table.repaint();
            }));
    };
    playButton.setButtonText (T ("Play (PART 1)"));
    playButton.setTooltip (T ("Create a user and put it on PART 1 of the current performance. Double-click on a program works too."));
    playButton.onClick = [this]
    {
        const int r = table.getSelectedRow();
        if (r < 0) return;
        if (combiMode && table.getNumSelectedRows() > 1) { loadCombisToList (table.getSelectedRows()); return; }
        if (combiMode) loadCombiRow (r); else playRow (r);
    };
    const char* modeNames[] = { "PROGRAMS", "COMBIS" };
    for (int b = 0; b < 2; ++b)
    {
        modeButtons[b].setButtonText (modeNames[b]);
        modeButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        modeButtons[b].setToggleState (b == 0, juce::dontSendNotification);
        modeButtons[b].onClick = [this, b] { setMode (b == 1); };
        addAndMakeVisible (modeButtons[b]);
    }
    for (auto* b : { &createSelected, &createAll, &playButton })
        addAndMakeVisible (b);

    help.setText (T ("Programs: double-click creates a user (OSC1/OSC2, zones, filter, EG...) and puts it on PART 1. "
                     "Combis: double-click creates a performance from 16 timbres; factory ROM programs are skipped."),
                  juce::dontSendNotification);
    help.setFont (juce::FontOptions (12.5f));
    help.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (help);
}

void KronosPage::loadPcg (const juce::File& f)
{
    juce::String err;
    if (! proc.loadKronosPcg (f, err))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Kronos", T ("Cannot load the PCG: ") + err);
    refresh();
}

int KronosPage::userFor (const Kronos::Program& p)
{
    return proc.userForKronosProgram (p);
}

void KronosPage::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (combiMode) loadCombiRow (row);
    else           playRow (row);
}

void KronosPage::setMode (bool combis)
{
    combiMode = combis;
    for (int b = 0; b < 2; ++b)
        modeButtons[b].setToggleState ((b == 1) == combis, juce::dontSendNotification);
    auto& h = table.getHeader();
    h.setColumnName (kcName, combis ? T ("Combi") : T ("Program"));
    h.setColumnName (kcOsc1, combis ? T ("Timbres (Play)") : T ("OSC1 multisample"));
    h.setColumnName (kcOsc2, combis ? T ("Factory (ROM)") : T ("OSC2 multisample"));
    h.setColumnName (kcState, combis ? T ("In list / channel") : T ("User"));
    h.setColumnWidth (kcState, combis ? 220 : 150);
    h.setColumnWidth (kcOsc1, combis ? 520 : 260);
    h.setColumnWidth (kcOsc2, combis ? 120 : 260);
    onlyUser.setButtonText (combis ? T ("Only combis with my samples") : T ("Only programs with my samples"));
    playButton.setButtonText (combis ? T ("Load into performance") : T ("Play (PART 1)"));
    playButton.setTooltip (combis ? T ("One combi becomes the current performance (16 parts); double-click works too. "
                                       "Several selected (Ctrl / Shift + click) are loaded at once into the performance list.")
                                  : T ("Create a user and put it on PART 1 of the current performance. Double-click on a program works too."));
    createSelected.setVisible (! combis);
    keepChannels.setVisible (combis);
    createAll.setVisible (! combis);
    search.setText ({}, juce::dontSendNotification);
    rebuildRows();
}

juce::String KronosPage::timbreSummary (const Kronos::Combi& c, int& romCount) const
{
    auto* pcg = proc.getKronosPcg();
    juce::StringArray parts;
    romCount = 0;
    for (int t = 0; t < 16; ++t)
    {
        const auto& tm = c.timbres[(size_t) t];
        if (! tm.play) continue;
        const auto* p = pcg != nullptr ? pcg->findProgram (tm.programBank, tm.programNumber) : nullptr;
        if (p == nullptr || ! proc.kronosProgramPlayable (*p)) { ++romCount; continue; }
        parts.add (juce::String (t + 1) + " " + p->name);
    }
    return parts.joinIntoString ("  ·  ");
}

juce::File KronosPage::findAutosampler()
{
    // trazi "Pikaso Autosampler.exe" (ili stari "Kronos Autosampler.exe") pored programa, u susednim folderima i na Desktopu
    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    juce::Array<juce::File> roots { exeDir, exeDir.getParentDirectory(), exeDir.getParentDirectory().getParentDirectory(),
                                    juce::File::getSpecialLocation (juce::File::userDesktopDirectory) };
    for (auto& r : roots)
    {
        if (! r.isDirectory()) continue;
        for (auto* nm : { "Pikaso Autosampler.exe", "Kronos Autosampler.exe" })
            for (auto& f : r.findChildFiles (juce::File::findFiles, false, nm))
                return f;
        for (auto& d : r.findChildFiles (juce::File::findDirectories, false))
        {
            for (auto* nm : { "Pikaso Autosampler.exe", "Kronos Autosampler.exe" })
                for (auto& f : d.findChildFiles (juce::File::findFiles, false, nm))
                    return f;
            for (auto& dd : d.findChildFiles (juce::File::findDirectories, false))
                for (auto* nm : { "Pikaso Autosampler.exe", "Kronos Autosampler.exe" })
                    for (auto& f : dd.findChildFiles (juce::File::findFiles, false, nm))
                        return f;
        }
    }
    return {};
}

void KronosPage::startAutosampler (const juce::String& args)
{
    const auto exe = findAutosampler();
    if (! exe.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Kronos",
            T ("Cannot find \"Pikaso Autosampler.exe\". Unzip PikasoAutosampler-Windows next to Pikaso Workstation or on the Desktop."));
        return;
    }
    exe.startAsProcess (args);
}

namespace
{
    // spisak zvukova koji fale: kucicom se bira sta da se presempluje
    struct MissingPanel : public juce::Component
    {
        MissingPanel (const juce::String& message, const std::vector<LiveSamplerProcessor::KronosMissing>& m, const juce::String& pcg)
            : items (m), pcgPath (pcg)
        {
            text.setText (message, juce::dontSendNotification);
            text.setJustificationType (juce::Justification::topLeft);
            text.setFont (juce::FontOptions (14.0f));
            addAndMakeVisible (text);
            for (auto& it : items)
            {
                auto* t = toggles.add (new juce::ToggleButton (it.label + "   (" + it.where + ")"));
                t->setToggleState (it.canResample, juce::dontSendNotification);
                t->setEnabled (it.canResample);
                if (! it.canResample) t->setTooltip ("Its samples are your own (KSC) and are missing in the library: they cannot be resampled from ROM.");
                holder.addAndMakeVisible (t);
            }
            view.setViewedComponent (&holder, false);
            addAndMakeVisible (view);
            for (auto* b : { &all, &none, &go, &close }) addAndMakeVisible (b);
            all.setButtonText ("Select all");
            none.setButtonText ("Select none");
            go.setButtonText ("Resample selected");
            close.setButtonText ("Not now");
            all.onClick = [this] { for (int i = 0; i < toggles.size(); ++i) if (items[(size_t) i].canResample) toggles[i]->setToggleState (true, juce::dontSendNotification); };
            none.onClick = [this] { for (auto* t : toggles) t->setToggleState (false, juce::dontSendNotification); };
            go.onClick = [this]
            {
                juce::String args = "--pcg \"" + pcgPath + "\"";
                int n = 0;
                for (int i = 0; i < toggles.size(); ++i)
                    if (toggles[i]->getToggleState()) { args << " --program " << juce::String (items[(size_t) i].bank) << ":" << items[(size_t) i].number; ++n; }
                if (n > 0) KronosPage::startAutosampler (args);
                dismiss();
            };
            close.onClick = [this] { dismiss(); };
            setSize (720, juce::jlimit (260, 640, 170 + 28 * (int) items.size()));
        }
        void dismiss() { if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0); }
        void resized() override
        {
            auto r = getLocalBounds().reduced (10);
            text.setBounds (r.removeFromTop (70));
            auto b = r.removeFromBottom (34);
            close.setBounds (b.removeFromRight (110));
            b.removeFromRight (8);
            go.setBounds (b.removeFromRight (170));
            all.setBounds (b.removeFromLeft (110));
            b.removeFromLeft (8);
            none.setBounds (b.removeFromLeft (110));
            r.removeFromBottom (8);
            view.setBounds (r);
            holder.setSize (r.getWidth() - 14, 28 * toggles.size());
            for (int i = 0; i < toggles.size(); ++i) toggles[i]->setBounds (0, i * 28, holder.getWidth(), 26);
        }
        std::vector<LiveSamplerProcessor::KronosMissing> items;
        juce::String pcgPath;
        juce::Label text;
        juce::Viewport view;
        juce::Component holder;
        juce::OwnedArray<juce::ToggleButton> toggles;
        juce::TextButton all, none, go, close;
    };
}

void KronosPage::offerMissing (const juce::String& message, const std::vector<LiveSamplerProcessor::KronosMissing>& missing, const juce::String& pcgPath)
{
    if (missing.empty()) { if (message.isNotEmpty()) juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Kronos", message); return; }
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new MissingPanel (message, missing, pcgPath));
    o.dialogTitle = "Sounds that are not resampled yet";
    o.dialogBackgroundColour = Pal::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void KronosPage::offerAutosample (const juce::String& message, const juce::String& args)
{
    juce::Component::SafePointer<KronosPage> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Kronos", message,
        T ("Resample from Kronos"), T ("Close"), nullptr,
        juce::ModalCallbackFunction::create ([safe, args] (int r)
        {
            if (safe == nullptr || r == 0) return;
            const auto exe = findAutosampler();
            if (! exe.existsAsFile())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Kronos",
                    T ("Cannot find \"Pikaso Autosampler.exe\". Unzip PikasoAutosampler-Windows next to Pikaso Workstation or on the Desktop."));
                return;
            }
            exe.startAsProcess (args);
        }));
}

void KronosPage::loadCombiRow (int row)
{
    auto* pcg = proc.getKronosPcg();
    if (pcg == nullptr || row < 0 || row >= (int) rows.size())
        return;
    const auto& combi = pcg->combis[(size_t) rows[(size_t) row]];
    std::vector<LiveSamplerProcessor::KronosMissing> missing;
    const auto skipped = proc.loadKronosCombi (combi, keepChannels.getToggleState(), &missing);
    editor.showTab (LiveSamplerEditor::TabPerformance);

    // odmah pitaj gde da se sacuva (prvo prazno mesto u trenutnoj listi)
    {
        const int bank = proc.getCurrentBank();
        int firstFree = 0;
        while (firstFree < kPerfPerBank && proc.getSlot (bank * kPerfPerBank + firstFree).used) ++firstFree;
        auto* aw = new juce::AlertWindow (T ("Save performance"),
            T ("Combi \"") + combi.name + T ("\" is loaded. Where should I save it?"), juce::MessageBoxIconType::QuestionIcon);
        juce::StringArray lists;
        for (int b = 0; b < kNumPerfBanks; ++b) lists.add (juce::String (b + 1) + "  " + proc.getBankName (b));
        aw->addComboBox ("list", lists, T ("List:"));
        aw->getComboBoxComponent ("list")->setSelectedItemIndex (bank);
        aw->addTextEditor ("slot", juce::String (juce::jmin (kPerfPerBank - 1, firstFree)).paddedLeft ('0', 3), T ("Slot (000-127):"));
        aw->addButton (T ("Save"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton (T ("Load only"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<KronosPage> safe (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r)
        {
            if (safe == nullptr || r != 1) return;
            auto& pr = safe->proc;
            const int list = aw->getComboBoxComponent ("list")->getSelectedItemIndex();
            const int slot = list * kPerfPerBank + juce::jlimit (0, kPerfPerBank - 1, aw->getTextEditorContents ("slot").getIntValue());
            auto store = [safe, slot, list]
            {
                if (safe == nullptr) return;
                safe->proc.storePerformance (slot);
                safe->proc.setCurrentBank (list);
                safe->table.repaint();
            };
            if (pr.getSlot (slot).used)
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("Save performance"),
                    T ("Slot ") + juce::String (list + 1) + "-" + juce::String (slot % kPerfPerBank).paddedLeft ('0', 3)
                        + T (" is \"") + pr.getSlot (slot).name + T ("\". Replace?"),
                    T ("Replace"), T ("Cancel"), nullptr, juce::ModalCallbackFunction::create ([store] (int ok) { if (ok) store(); }));
            else store();
        }), true);
    }
    if (skipped.isEmpty())
        return;
    offerMissing ("These sounds of \"" + combi.name + "\" are not resampled yet. Their parts are in the performance anyway (marked with a warning sign) "
                  "and they get the sound by themselves once it is resampled.\nCheck which ones to resample now:", missing, pcg->file.getFullPathName());
}

void KronosPage::loadCombisToList (const juce::SparseSet<int>& sel)
{
    auto* pcg = proc.getKronosPcg();
    if (pcg == nullptr) return;
    std::vector<int> combis;
    for (int i = 0; i < sel.size(); ++i)
        if (sel[i] >= 0 && sel[i] < (int) rows.size()) combis.push_back (rows[(size_t) sel[i]]);
    if (combis.empty()) return;

    auto* aw = new juce::AlertWindow (T ("Load combis into a list"),
        juce::String ((int) combis.size()) + T (" combis become performances in a list. Choose the list and the start slot."), juce::MessageBoxIconType::QuestionIcon);
    juce::StringArray lists;
    for (int b = 0; b < kNumPerfBanks; ++b) lists.add (juce::String (b + 1) + "  " + proc.getBankName (b));
    aw->addComboBox ("list", lists, T ("List:"));
    aw->getComboBoxComponent ("list")->setSelectedItemIndex (proc.getCurrentBank());
    aw->addTextEditor ("from", "000", T ("From slot (000-127):"));
    aw->addComboBox ("mode", { T ("Only empty slots (used ones are skipped)"), T ("In order, over existing ones") }, T ("Slots:"));
    aw->getComboBoxComponent ("mode")->setSelectedItemIndex (0);
    aw->addButton (T ("Load"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (T ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<KronosPage> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, combis] (int r)
    {
        if (safe == nullptr || r != 1) return;
        auto& pr = safe->proc;
        auto* pcg2 = pr.getKronosPcg();
        if (pcg2 == nullptr) return;
        const int list = aw->getComboBoxComponent ("list")->getSelectedItemIndex();
        const bool onlyEmpty = aw->getComboBoxComponent ("mode")->getSelectedItemIndex() == 0;
        int slot = list * kPerfPerBank + juce::jlimit (0, kPerfPerBank - 1, aw->getTextEditorContents ("from").getIntValue());
        const int end = (list + 1) * kPerfPerBank;
        int loaded = 0;
        juce::StringArray skippedAll, notPlaced;
        std::vector<LiveSamplerProcessor::KronosMissing> missing;
        for (int ci : combis)
        {
            const auto& combi = pcg2->combis[(size_t) ci];
            while (onlyEmpty && slot < end && pr.getSlot (slot).used) ++slot;
            if (slot >= end) { notPlaced.add (combi.id() + " " + combi.name); continue; }
            const auto skipped = pr.loadKronosCombi (combi, safe->keepChannels.getToggleState(), &missing);
            pr.storePerformance (slot);
            ++slot;
            ++loaded;
            if (! skipped.isEmpty()) skippedAll.add (combi.id() + " " + combi.name + ": " + juce::String (skipped.size()) + " timbre");
        }
        pr.setCurrentBank (list);
        safe->editor.showTab (LiveSamplerEditor::TabLists);
        juce::String msg = T ("Loaded ") + juce::String (loaded) + T (" combis into list ") + juce::String (list + 1) + ".";
        if (! notPlaced.isEmpty()) msg << T ("\n\nNo room in the list for: ") << notPlaced.joinIntoString (", ");
        if (! missing.empty())
            msg << "\nSounds that are not resampled yet stay in their parts (warning sign) and get the sound by themselves once resampled. "
                   "Check which ones to resample now:";
        safe->offerMissing (msg, missing, pcg2->file.getFullPathName());
    }), true);
}

void KronosPage::playRow (int row)
{
    auto* pcg = proc.getKronosPcg();
    if (pcg == nullptr || row < 0 || row >= (int) rows.size())
        return;
    const auto& prog = pcg->programs[(size_t) rows[(size_t) row]];
    const int u = userFor (prog);
    if (u < 0)
    {
        bool rom = false;
        for (auto& o : prog.osc) for (auto& z : o) rom = rom || z.romKey.isNotEmpty();
        if (prog.exi)
            offerAutosample (T ("Program \"") + prog.name + T ("\" is an EXi program (") + prog.engineText()
                             + T (" synthesis) and is not resampled yet.\n\nResample it from the Kronos (Pikaso Autosampler)? The whole sound of the program is recorded, every third note, at several velocities."),
                             "--pcg \"" + pcg->file.getFullPathName() + "\" --program " + juce::String (prog.bankId) + ":" + juce::String (prog.number));
        else if (prog.drumKit)
            offerAutosample ("Program \"" + prog.name + "\" is a drum kit and is not resampled yet.\n\nResample it from the Kronos (Pikaso Autosampler)? "
                             "Every key is recorded separately (the drum kit must be the same on the Kronos).",
                             "--pcg \"" + pcg->file.getFullPathName() + "\" --program " + juce::String (prog.bankId) + ":" + juce::String (prog.number));
        else if (rom)
            offerAutosample (T ("Program \"") + prog.name + T ("\" uses factory ROM multisamples that are not resampled yet.\n\nResample them from the Kronos (Pikaso Autosampler)?"),
                             "--pcg \"" + pcg->file.getFullPathName() + "\" --program " + juce::String (prog.bankId) + ":" + juce::String (prog.number));
        else
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Kronos",
                T ("The samples of this program are not in the library. Check that the KSC collection is in a library folder (SETTINGS)."));
        return;
    }
    auto perf = proc.getPerformance();
    perf.parts[0].on = true;
    perf.parts[0].user = proc.getUser (u).name;
    proc.updatePerformance (perf);
    table.repaint();
}

void KronosPage::rebuildRows()
{
    rows.clear();
    auto* pcg = proc.getKronosPcg();
    shownPcg = pcg;
    if (pcg != nullptr)
    {
        const auto q = search.getText().trim();
        if (combiMode)
        {
            for (size_t i = 0; i < pcg->combis.size(); ++i)
            {
                const auto& c = pcg->combis[i];
                int rom = 0;
                const auto sum = timbreSummary (c, rom);
                if (onlyUser.getToggleState() && sum.isEmpty()) continue;
                if (q.isNotEmpty() && ! c.name.containsIgnoreCase (q) && ! sum.containsIgnoreCase (q)) continue;
                rows.push_back ((int) i);
            }
        }
        else
        for (size_t i = 0; i < pcg->programs.size(); ++i)
        {
            const auto& p = pcg->programs[i];
            if (! p.hd1 && ! p.exi) continue;
            if (onlyUser.getToggleState() && ! proc.kronosProgramPlayable (p)) continue;
            if (q.isNotEmpty() && ! p.name.containsIgnoreCase (q)) continue;
            rows.push_back ((int) i);
        }
    }
    table.updateContent();
    table.repaint();
}

void KronosPage::refresh()
{
    if (proc.getKronosPcg() == nullptr && proc.getKronosFile().existsAsFile())
    {
        juce::String err;
        proc.loadKronosPcg (proc.getKronosFile(), err);
    }

    auto* pcg = proc.getKronosPcg();
    if (pcg != shownPcg)
        rebuildRows();
    else
        table.repaint();

    if (pcg == nullptr)
    {
        fileLabel.setText (T ("No PCG loaded"), juce::dontSendNotification);
        infoLabel.setText (T ("Load a PCG from the Kronos. The KSC file and the sample folder should be next to it."), juce::dontSendNotification);
        return;
    }

    int hd1 = 0, user = 0, exi = 0;
    for (auto& p : pcg->programs)
    {
        if (p.hd1) ++hd1;
        if (p.exi) ++exi;
        if (p.usesUser()) ++user;
    }
    fileLabel.setText (pcg->file.getFileName(), juce::dontSendNotification);
    infoLabel.setText (juce::String ((int) pcg->programs.size()) + T (" programs (") + juce::String (hd1) + T (" HD-1, ") + juce::String (exi) + T (" EXi), ")
                       + juce::String (user) + T (" with your samples  |  ") + juce::String ((int) pcg->combis.size()) + T (" combis  |  ")
                       + juce::String (pcg->drumKits) + T (" drum kits") + (proc.isScanning() ? T ("  |  scanning samples...") : juce::String()),
                       juce::dontSendNotification);
}

juce::String KronosPage::msText (const Kronos::Zone& z) const
{
    if (! z.user)
    {
        if (! z.used) return "-";
        const auto r = proc.resolveKronosZone (z);
        if (r.isNotEmpty()) return "ROM: " + r.fromLastOccurrenceOf ("/", false, false);
        // ime iz Voice Name Liste (ROM / EXs, mono / stereo)
        juce::MemoryBlock mb;
        mb.loadFromHexString (z.romKey.fromFirstOccurrenceOf ("rom:", false, false));
        const auto* b = (const juce::uint8*) mb.getData();
        if (mb.getSize() >= 20)
        {
            const int type = b[0] & 3, bankByte = b[16], bank = bankByte >> 1;
            juce::String t = type == 2 ? juce::String ("WaveSeq") : bank == 0 ? juce::String ("ROM") : "EXs" + juce::String (bank - 1);
            if ((bankByte & 1) && type != 2) t << " St";
            t << " " << juce::String (z.ms);
            if (type == 1) if (auto nm = kronosMsName (bankByte, z.ms); nm.isNotEmpty()) t << " " << nm;
            return t;
        }
        return "ROM MS" + juce::String (z.ms);
    }
    const auto inst = proc.findKronosMultisample (z.uuid, z.ms);
    if (inst.isEmpty()) return "MS" + juce::String (z.ms) + T ("  (not in library)");
    return inst.fromLastOccurrenceOf ("/", false, false);
}

int KronosPage::getNumRows() { return (int) rows.size(); }

void KronosPage::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    if (selected) g.fillAll (Pal::lcdText.withAlpha (0.25f));
    else if (row % 2) g.fillAll (Pal::panel.withAlpha (0.5f));
}

void KronosPage::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    auto* pcg = proc.getKronosPcg();
    if (pcg == nullptr || row < 0 || row >= (int) rows.size()) return;

    juce::String t;
    auto c = Pal::text;
    if (combiMode)
    {
        const auto& cb = pcg->combis[(size_t) rows[(size_t) row]];
        int rom = 0;
        switch (col)
        {
            case kcBank: t = cb.bank; break;
            case kcNum:  t = juce::String (cb.number).paddedLeft ('0', 3); break;
            case kcName: t = cb.name; break;
            case kcOsc1: t = timbreSummary (cb, rom); if (t.isEmpty()) { t = "-"; c = Pal::dim; } break;
            case kcOsc2: timbreSummary (cb, rom); t = rom > 0 ? juce::String (rom) + T (" timbre") : "-"; c = Pal::dim; break;
            case kcState:
            {
                juce::StringArray ch;
                for (auto& tm : cb.timbres)
                {
                    const auto s = tm.channel == 0 || tm.channel == pcg->globalChannel ? juce::String ("Gch") : juce::String (tm.channel);
                    if (tm.play && ! ch.contains (s)) ch.add (s);
                }
                // gde je ovaj kombi u listama (po kombiju od kog je performans napravljen, ili po imenu)
                const auto key = juce::String (cb.bankId) + ":" + juce::String (cb.number);
                juce::StringArray where;
                for (int sl = 0; sl < kNumPerfSlots && where.size() < 3; ++sl)
                {
                    const auto& ps = proc.getSlot (sl);
                    // samo iz ovog PCG-a (stariji performansi bez oznake PCG-a: po kombiju ili imenu)
                    const bool samePcg = ps.kronosPcg.isEmpty() || ps.kronosPcg == pcg->file.getFileName();
                    if (ps.used && samePcg && (ps.kronosSource == key || (ps.kronosSource.isEmpty() && ps.name == cb.name)))
                        where.add (juce::String (sl / kPerfPerBank + 1) + "-" + juce::String (sl % kPerfPerBank).paddedLeft ('0', 3));
                }
                const auto& cur = proc.getPerformance();
                if (where.isEmpty() && cur.kronosSource == key && (cur.kronosPcg.isEmpty() || cur.kronosPcg == pcg->file.getFileName()))
                {
                    t = T ("● loaded, not saved");
                    c = Pal::accent;
                }
                else if (! where.isEmpty())
                {
                    t = T ("✓ ") + where.joinIntoString (", ") + "   " + ch.joinIntoString (", ");
                    c = juce::Colour (0xff3ddc84);
                }
                else
                {
                    t = ch.joinIntoString (", ");
                    c = Pal::dim;
                }
                break;
            }
            default: break;
        }
        g.setColour (c);
        g.setFont (juce::FontOptions (13.5f));
        g.drawText (t, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
        return;
    }
    const auto& p = pcg->programs[(size_t) rows[(size_t) row]];
    switch (col)
    {
        case kcBank: t = p.bank; break;
        case kcNum:  t = juce::String (p.number).paddedLeft ('0', 3); break;
        case kcName: t = p.name; break;
        case kcOsc1:
        case kcOsc2:
        {
            if (p.exi)
            {
                const auto inst = proc.resolveKronosExi (p);
                if (col == kcOsc1) t = "EXi " + p.engineText();
                else t = inst.isNotEmpty() ? T ("resampled") : T ("not resampled");
                if (inst.isEmpty()) c = Pal::dim;
                break;
            }
            const int o = col == kcOsc1 ? 0 : 1;
            const int z = p.firstUserZone (o);
            t = z >= 0 ? msText (p.osc[(size_t) o][(size_t) z]) : (p.osc[(size_t) o][0].used ? msText (p.osc[(size_t) o][0]) : "-");
            if (z < 0 && ! (p.osc[(size_t) o][0].used && proc.resolveKronosZone (p.osc[(size_t) o][0]).isNotEmpty())) c = Pal::dim;
            int extra = 0;
            for (auto& zz : p.osc[(size_t) o]) if (zz.user) ++extra;
            if (extra > 1) t << "  (+" << (extra - 1) << ")";
            break;
        }
        case kcState:
        {
            // user napravljen od ovog programa iz ovog PCG-a (stariji useri bez oznake: po imenu)
            const auto key = juce::String (p.bankId) + ":" + juce::String (p.number);
            const auto pcgName = pcg->file.getFileName();
            bool mine = false, otherPcg = false;
            for (int u = 0; u < proc.getNumUsers(); ++u)
            {
                const auto& us = proc.getUser (u);
                if (us.kronosSource == key && us.kronosPcg == pcgName) mine = true;
                else if (us.kronosSource.isEmpty() && us.name == p.name) mine = true;
                else if (us.name == p.name) otherPcg = true;
            }
            t = mine ? T ("✓ created") : otherPcg ? T ("same name from another PCG") : juce::String();
            c = mine ? juce::Colour (0xff3ddc84) : Pal::dim;
            break;
        }
        default: break;
    }
    g.setColour (c);
    g.setFont (juce::FontOptions (13.5f));
    g.drawText (t, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

void KronosPage::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
}

void KronosPage::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    auto top = r.removeFromTop (32);
    loadButton.setBounds (top.removeFromLeft (190));
    top.removeFromLeft (14);
    fileLabel.setBounds (top.removeFromLeft (260));
    infoLabel.setBounds (top);
    r.removeFromTop (8);
    auto filt = r.removeFromTop (28);
    modeButtons[0].setBounds (filt.removeFromLeft (110).reduced (1, 0));
    modeButtons[1].setBounds (filt.removeFromLeft (110).reduced (1, 0));
    filt.removeFromLeft (14);
    onlyUser.setBounds (filt.removeFromLeft (300));
    search.setBounds (filt.removeFromLeft (260));
    filt.removeFromLeft (14);
    keepChannels.setBounds (filt.removeFromLeft (260));
    filt.removeFromLeft (8);
    fmToggle.setBounds (filt.removeFromLeft (190));
    r.removeFromTop (8);
    help.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (6);
    auto btns = r.removeFromBottom (32);
    createSelected.setBounds (btns.removeFromLeft (230));
    btns.removeFromLeft (8);
    createAll.setBounds (btns.removeFromLeft (260));
    btns.removeFromLeft (8);
    playButton.setBounds (btns.removeFromLeft (170));
    r.removeFromBottom (8);
    table.setBounds (r);
}

//==============================================================================
//  ListsPage
//==============================================================================
ListsPage::ListsPage (LiveSamplerProcessor& p, LiveSamplerEditor& e) : proc (p), editor (e)
{
    for (int b = 0; b < kNumPerfBanks; ++b)
    {
        bankButtons[b].setButtonText (juce::String (b + 1));
        bankButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::lcdText);
        bankButtons[b].onClick = [this, b] { proc.setCurrentBank (b); };
        addAndMakeVisible (bankButtons[b]);
    }
    writeButton.setButtonText ("WRITE");
    writeButton.setClickingTogglesState (true);
    writeButton.setColour (juce::TextButton::buttonOnColourId, Pal::red);
    writeButton.setTooltip (T ("Turn WRITE on, then click a slot to save the current performance"));
    writeButton.onClick = [this] { repaint(); };
    addAndMakeVisible (writeButton);

    styleLabel (nameLabel, T ("LIST NAME"), 12.5f);
    addAndMakeVisible (nameLabel);
    nameEdit.setEditable (true);
    nameEdit.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    nameEdit.setColour (juce::Label::backgroundColourId, Pal::bg);
    nameEdit.setColour (juce::Label::textColourId, Pal::lcdText);
    nameEdit.onTextChange = [this] { proc.setBankName (proc.getCurrentBank(), nameEdit.getText()); };
    addAndMakeVisible (nameEdit);

    styleLabel (filterLabel, T ("CATEGORY"), 12.5f);
    addAndMakeVisible (filterLabel);
    catFilter.onChange = [this] { repaint(); };
    addAndMakeVisible (catFilter);
    categoriesButton.setButtonText (T ("CATEGORIES..."));
    categoriesButton.setTooltip (T ("Edit, add, delete or reorder categories"));
    categoriesButton.onClick = [this] { categoriesMenu(); };
    addAndMakeVisible (categoriesButton);
    missingLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    missingLabel.setColour (juce::Label::textColourId, juce::Colour (0xffff8a3d));
    addAndMakeVisible (missingLabel);

    help.setText (T ("Click: load. Drag: move (onto a taken slot = swap, onto a list button = other list, down onto a set list button = put it there). WRITE + click: save. Right-click: menu. "
                     "Orange frame = performance without a category. "
                     "MIDI: Bank Select (CC0) selects the list, Program Change 0-127 the performance."), juce::dontSendNotification);
    help.setFont (juce::FontOptions (12.0f));
    help.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (help);
}

void ListsPage::refresh()
{
    const int bank = proc.getCurrentBank();
    for (int b = 0; b < kNumPerfBanks; ++b)
    {
        bankButtons[b].setToggleState (b == bank, juce::dontSendNotification);
        bankButtons[b].setTooltip (proc.getBankName (b));
    }
    if (! nameEdit.isBeingEdited())
        nameEdit.setText (proc.getBankName (bank), juce::dontSendNotification);

    // filter kategorija (zadrzava izbor ako ime i dalje postoji)
    const auto& cats = proc.getCategories();
    const int sel = catFilter.getSelectedId();
    catFilter.clear (juce::dontSendNotification);
    catFilter.addItem (T ("All"), 1);
    catFilter.addItem (T ("No category"), 2);
    catFilter.addSeparator();
    for (int c = 0; c < cats.size(); ++c)
        catFilter.addItem (cats[c], 10 + c);
    catFilter.setSelectedId (sel > 0 && catFilter.indexOfItemId (sel) >= 0 ? sel : 1, juce::dontSendNotification);

    // koliko performansa nema kategoriju: u ovoj listi i u svim listama; tacka na dugmetu liste
    int here = 0, all = 0;
    for (int b = 0; b < kNumPerfBanks; ++b)
    {
        int n = 0;
        for (int i = 0; i < kPerfPerBank; ++i)
        {
            const auto& s = proc.getSlot (b * kPerfPerBank + i);
            if (s.used && proc.categoryIndex (s) < 0) ++n;
        }
        all += n;
        if (b == bank) here = n;
        bankButtons[b].setButtonText (juce::String (b + 1) + (n > 0 ? juce::String::fromUTF8 (" \xe2\x80\xa2") : juce::String()));
        bankButtons[b].setTooltip (proc.getBankName (b) + (n > 0 ? "  (" + juce::String (n) + T (" without category)") : juce::String()));
    }
    missingLabel.setText (all == 0 ? juce::String() : juce::String (here) + T (" without category in this list, ") + juce::String (all) + T (" in all lists"),
                          juce::dontSendNotification);
    repaint();
}

void ListsPage::armWrite()
{
    writeButton.setToggleState (true, juce::dontSendNotification);
    repaint();
}

bool ListsPage::passesFilter (const Performance& s) const
{
    const int id = catFilter.getSelectedId();
    if (id <= 1) return true;
    if (id == 2) return proc.categoryIndex (s) < 0;
    return proc.categoryIndex (s) == id - 10;
}

juce::Rectangle<int> ListsPage::gridArea() const
{
    auto r = getLocalBounds().reduced (12, 10);
    r.removeFromTop (34 + 8 + 30 + 8);
    r.removeFromBottom (24);
    return r;
}

int ListsPage::cellAt (juce::Point<int> p) const
{
    auto g = gridArea();
    if (! g.contains (p)) return -1;
    const int cols = 8, rows = 16;
    const int c = (p.x - g.getX()) * cols / g.getWidth();
    const int r = (p.y - g.getY()) * rows / g.getHeight();
    return juce::jlimit (0, 127, c * rows + r);   // po kolonama: 1-16, 17-32...
}

void ListsPage::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    auto area = gridArea();
    const int cols = 8, rows = 16;
    const float cw = area.getWidth() / (float) cols, ch = area.getHeight() / (float) rows;
    const int bank = proc.getCurrentBank();
    const bool writing = writeButton.getToggleState();
    g.setFont (juce::FontOptions (12.5f));
    for (int i = 0; i < kPerfPerBank; ++i)
    {
        const int c = i / rows, r = i % rows;
        auto cell = juce::Rectangle<float> (area.getX() + c * cw, area.getY() + r * ch, cw, ch).reduced (1.5f);
        const int slot = bank * kPerfPerBank + i;
        const auto& s = proc.getSlot (slot);
        const bool current = proc.getCurrentSlot() == slot;
        auto col = current ? Pal::lcdText : (s.used ? Pal::panelHi.brighter (0.15f) : Pal::bg);
        if (writing) col = col.interpolatedWith (Pal::red, 0.25f);
        const bool dimmed = s.used && ! passesFilter (s);
        if (dimmed) col = col.interpolatedWith (Pal::bg, 0.7f);
        g.setColour (col);
        g.fillRoundedRectangle (cell, 3.0f);
        auto textArea = cell.reduced (5.0f, 0.0f);
        if (s.used)
        {
            // kategorija desno (skraceno), ili narandzast okvir ako je nema
            const int ci = proc.categoryIndex (s);
            const bool missing = ci < 0;
            const auto cat = missing ? T ("NO CAT.") : s.category;
            if (! missing)
            {
                // boja kategorije (kao u Live Kontroleru): traka levo
                g.setColour (proc.categoryColour (ci).withAlpha (dimmed ? 0.3f : 1.0f));
                g.fillRoundedRectangle (cell.withWidth (5.0f), 2.0f);
            }
            const float tw = juce::jmin (textArea.getWidth() * 0.4f, 80.0f);
            auto catArea = textArea.removeFromRight (tw);
            g.setFont (juce::FontOptions (10.5f, missing ? juce::Font::bold : juce::Font::plain));
            g.setColour (missing ? juce::Colour (0xffff8a3d) : (current ? juce::Colours::black.withAlpha (0.6f) : Pal::dim));
            if (dimmed) g.setColour (Pal::dim.withAlpha (0.4f));
            g.drawText (cat, catArea, juce::Justification::centredRight, true);
            if (missing && ! dimmed)
            {
                g.setColour (juce::Colour (0xffff8a3d));
                g.drawRoundedRectangle (cell.reduced (0.5f), 3.0f, 1.6f);
            }
            g.setFont (juce::FontOptions (12.5f));
        }
        g.setColour (current ? juce::Colours::black : (s.used ? (dimmed ? Pal::dim.withAlpha (0.5f) : Pal::text) : Pal::dim.withAlpha (0.6f)));
        g.drawText (juce::String (i).paddedLeft ('0', 3) + "  " + (s.used ? s.name : juce::String()),
                    textArea, juce::Justification::centredLeft, true);
        if (i == dropCell)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (cell, 3.0f, 2.5f);
        }
    }
}

juce::Rectangle<float> ListsPage::cellRect (int i) const
{
    auto area = gridArea();
    const int cols = 8, rows = 16;
    const float cw = area.getWidth() / (float) cols, ch = area.getHeight() / (float) rows;
    return juce::Rectangle<float> (area.getX() + (i / rows) * cw, area.getY() + (i % rows) * ch, cw, ch).reduced (1.5f);
}

void ListsPage::mouseDown (const juce::MouseEvent& e)
{
    dragStarted = false;
    pressCell = -1;
    const int i = cellAt (e.getPosition());
    if (i < 0) return;
    if (e.mods.isPopupMenu()) { cellMenu (i); return; }
    pressCell = i;   // klik = ucitaj (na pustanje), povlacenje = premesti
}

void ListsPage::mouseDrag (const juce::MouseEvent& e)
{
    if (pressCell < 0 || dragStarted || e.getDistanceFromDragStart() < 8 || writeButton.getToggleState()) return;
    const int slot = proc.getCurrentBank() * kPerfPerBank + pressCell;
    if (! proc.getSlot (slot).used) return;
    auto* dc = juce::DragAndDropContainer::findParentDragContainerFor (this);
    if (dc == nullptr) return;
    dragStarted = true;
    // slika koja prati mis: ime performansa
    const auto r = cellRect (pressCell).toNearestInt();
    juce::Image img (juce::Image::ARGB, juce::jmax (1, r.getWidth()), juce::jmax (1, r.getHeight()), true);
    {
        juce::Graphics g (img);
        g.setColour (Pal::accent.withAlpha (0.9f));
        g.fillRoundedRectangle (img.getBounds().toFloat(), 4.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (juce::String (pressCell).paddedLeft ('0', 3) + "  " + proc.getSlot (slot).name, img.getBounds().reduced (6, 0), juce::Justification::centredLeft, true);
    }
    const juce::Point<int> offset (r.getX() - e.getMouseDownX(), r.getY() - e.getMouseDownY());
    dc->startDragging ("perf:" + juce::String (slot), this, juce::ScaledImage (img), false, &offset);
}

void ListsPage::mouseUp (const juce::MouseEvent& e)
{
    const int i = pressCell;
    pressCell = -1;
    if (i < 0 || dragStarted || e.mods.isPopupMenu() || cellAt (e.getPosition()) != i) { dragStarted = false; return; }
    const int slot = proc.getCurrentBank() * kPerfPerBank + i;
    if (writeButton.getToggleState())
    {
        proc.storePerformance (slot);
        writeButton.setToggleState (false, juce::dontSendNotification);
    }
    else
    {
        proc.recallPerformance (slot);
    }
    repaint();
}

void ListsPage::itemDragMove (const SourceDetails& d)
{
    // preko dugmeta liste: prebaci se na tu listu, pa moze da se spusti tamo
    for (int b = 0; b < kNumPerfBanks; ++b)
        if (bankButtons[b].getBounds().contains (d.localPosition) && proc.getCurrentBank() != b)
        {
            proc.setCurrentBank (b);
            refresh();
        }
    const int c = cellAt (d.localPosition);
    if (c != dropCell) { dropCell = c; repaint(); }
}

void ListsPage::itemDropped (const SourceDetails& d)
{
    const int from = d.description.toString().fromFirstOccurrenceOf ("perf:", false, false).getIntValue();
    const int c = cellAt (d.localPosition);
    dropCell = -1;
    dragStarted = false;
    if (c >= 0)
    {
        const int to = proc.getCurrentBank() * kPerfPerBank + c;
        proc.swapPerformanceSlots (from, to);   // zauzeto mesto: zamene se
    }
    repaint();
}

void ListsPage::cellMenu (int i)
{
    const int slot = proc.getCurrentBank() * kPerfPerBank + i;
    const bool used = proc.getSlot (slot).used;
    juce::PopupMenu m;
    {
        const auto& sp = proc.getSlot (slot);
        if (used && sp.kronosSource.isNotEmpty())
        {
            // iz kog PCG-a (seta) i kog kombija je ovaj performans
            const auto bankId = (juce::uint32) sp.kronosSource.upToFirstOccurrenceOf (":", false, false).getLargeIntValue();
            const int num = sp.kronosSource.fromFirstOccurrenceOf (":", false, false).getIntValue();
            m.addSectionHeader (T ("From: ") + (sp.kronosPcg.isNotEmpty() ? sp.kronosPcg : T ("Kronos PCG")) + "  "
                                + Kronos::combiBankName (bankId) + " " + juce::String (num).paddedLeft ('0', 3));
        }
    }
    m.addItem (1, T ("Load"), used);
    {
        juce::PopupMenu cm;
        const auto& cats = proc.getCategories();
        const int cur = proc.categoryIndex (proc.getSlot (slot));
        for (int c = 0; c < cats.size(); ++c)
            cm.addItem (1000 + c, cats[c], true, cur == c);
        cm.addSeparator();
        cm.addItem (999, T ("No category"), true, cur < 0);
        cm.addItem (998, T ("Edit categories..."));
        m.addSubMenu (T ("Category"), cm, used);
    }
    m.addItem (2, T ("Write the current performance here"));
    m.addItem (5, T ("New empty (initialized) performance here"));
    m.addItem (3, T ("Add to set list (first empty button)"), used);
    m.addSeparator();
    m.addItem (4, T ("Delete"), used);
    juce::Component::SafePointer<ListsPage> safe (this);
    m.showMenuAsync ({}, [safe, slot] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& pr = safe->proc;
        if (r >= 1000) pr.setSlotCategory (slot, r - 1000);
        if (r == 999) pr.setSlotCategory (slot, -1);
        if (r == 998) { safe->editCategories(); return; }
        if (r == 1) pr.recallPerformance (slot);
        if (r == 2) pr.storePerformance (slot);
        if (r == 4) pr.clearPerformanceSlot (slot);
        if (r == 5)
        {
            if (pr.getSlot (slot).used)
            {
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("New performance"),
                    T ("This slot holds \"") + pr.getSlot (slot).name + T ("\". Replace it with an empty performance?"),
                    T ("Replace"), T ("Cancel"), nullptr,
                    juce::ModalCallbackFunction::create ([safe, slot] (int ok)
                    {
                        if (safe == nullptr || ok == 0) return;
                        safe->proc.initPerformanceSlot (slot);
                        safe->proc.recallPerformance (slot);
                    }));
                return;
            }
            pr.initPerformanceSlot (slot);
            pr.recallPerformance (slot);
        }
        if (r == 3)
        {
            auto sl = pr.getSetList (pr.getCurrentSetList());
            for (auto& s : sl.slots)
                if (s.perf < 0) { s.perf = slot; break; }
            pr.updateSetList (pr.getCurrentSetList(), sl);
        }
        safe->repaint();
    });
}

void ListsPage::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto top = r.removeFromTop (34);
    writeButton.setBounds (top.removeFromRight (100));
    top.removeFromRight (10);
    const int bw = top.getWidth() / kNumPerfBanks;
    for (auto& b : bankButtons)
        b.setBounds (top.removeFromLeft (bw).reduced (2, 0));
    r.removeFromTop (8);
    auto nameRow = r.removeFromTop (30);
    nameLabel.setBounds (nameRow.removeFromLeft (90));
    nameEdit.setBounds (nameRow.removeFromLeft (300).reduced (0, 1));
    nameRow.removeFromLeft (16);
    filterLabel.setBounds (nameRow.removeFromLeft (84));
    catFilter.setBounds (nameRow.removeFromLeft (190).reduced (0, 1));
    nameRow.removeFromLeft (8);
    categoriesButton.setBounds (nameRow.removeFromLeft (120).reduced (0, 1));
    nameRow.removeFromLeft (12);
    missingLabel.setBounds (nameRow);
    help.setBounds (r.removeFromBottom (22));
}

//==============================================================================
//  Uredjivanje kategorija: preimenuj (dupli klik), dodaj, obrisi, pomeri; ili preuzmi sa Kronosa
//==============================================================================
namespace
{
    class CategoryEditor : public juce::Component, private juce::ListBoxModel
    {
    public:
        CategoryEditor (LiveSamplerProcessor& p) : proc (p)
        {
            names = proc.getCategories();
            colours = proc.getCategoryColours();
            colours.resize ((size_t) names.size(), juce::Colour (0xff8a93a6));
            for (int i = 0; i < names.size(); ++i) origin.push_back (i);

            list.setModel (this);
            list.setRowHeight (26);
            list.setColour (juce::ListBox::backgroundColourId, Pal::bg);
            addAndMakeVisible (list);

            auto btn = [this] (juce::TextButton& b, const juce::String& t, std::function<void()> f)
            {
                b.setButtonText (t);
                b.onClick = std::move (f);
                addAndMakeVisible (b);
            };
            btn (addB, T ("Add"), [this]
            {
                names.add (T ("New category"));
                colours.push_back (juce::Colour (0xff8a93a6));
                origin.push_back (-1);
                list.updateContent();
                list.selectRow (names.size() - 1);
                renameRow (names.size() - 1);
            });
            btn (renB, T ("Rename"), [this] { renameRow (list.getSelectedRow()); });
            btn (delB, T ("Delete"), [this]
            {
                const int r = list.getSelectedRow();
                if (r < 0 || r >= names.size()) return;
                names.remove (r);
                colours.erase (colours.begin() + r);
                origin.erase (origin.begin() + r);
                list.updateContent();
                list.repaint();
            });
            btn (upB, T ("Up"), [this] { move (-1); });
            btn (downB, T ("Down"), [this] { move (1); });
            btn (kronosB, T ("Colour..."), [this] { pickColour (list.getSelectedRow()); });
            btn (factoryB, T ("Like Live Kontroler"), [this]
            {
                names = LiveSamplerProcessor::defaultCategories();
                colours = LiveSamplerProcessor::defaultCategoryColours();
                origin.clear();
                for (int k = 0; k < names.size(); ++k) origin.push_back (proc.getCategories().indexOf (names[k], true));
                list.updateContent();
                list.repaint();
            });
            btn (okB, "OK", [this]
            {
                std::vector<int> remap ((size_t) proc.getCategories().size(), -1);
                for (int i = 0; i < (int) origin.size(); ++i)
                    if (origin[(size_t) i] >= 0 && origin[(size_t) i] < (int) remap.size())
                        remap[(size_t) origin[(size_t) i]] = i;
                juce::StringArray clean;
                for (auto& n : names) clean.add (n.trim().isEmpty() ? T ("No name") : n.trim());
                proc.setCategories (clean, colours, remap);
                close();
            });
            btn (cancelB, T ("Cancel"), [this] { close(); });
            okB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f7d4a));
            setSize (460, 520);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10);
            auto bottom = r.removeFromBottom (32);
            cancelB.setBounds (bottom.removeFromRight (100));
            bottom.removeFromRight (8);
            okB.setBounds (bottom.removeFromRight (100));
            r.removeFromBottom (8);
            auto side = r.removeFromRight (160);
            r.removeFromRight (8);
            list.setBounds (r);
            for (auto* b : { &addB, &renB, &delB, &upB, &downB })
            {
                b->setBounds (side.removeFromTop (30));
                side.removeFromTop (6);
            }
            side.removeFromTop (16);
            kronosB.setBounds (side.removeFromTop (30));
            side.removeFromTop (6);
            factoryB.setBounds (side.removeFromTop (30));
        }

        void paint (juce::Graphics& g) override { g.fillAll (Pal::panel); }

    private:
        int getNumRows() override { return names.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
        {
            if (sel) g.fillAll (Pal::lcdText.withAlpha (0.25f));
            g.setColour (Pal::dim);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (juce::String (row + 1).paddedLeft ('0', 2), 6, 0, 28, h, juce::Justification::centredLeft);
            if (row < (int) colours.size())
            {
                g.setColour (colours[(size_t) row]);
                g.fillRoundedRectangle (34.0f, 5.0f, 16.0f, (float) h - 10.0f, 3.0f);
            }
            g.setColour (Pal::text);
            g.setFont (juce::FontOptions (14.0f));
            g.drawText (names[row], 58, 0, w - 64, h, juce::Justification::centredLeft, true);
        }
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { renameRow (row); }

        void renameRow (int row)
        {
            if (row < 0 || row >= names.size()) return;
            auto* aw = new juce::AlertWindow (T ("Category"), T ("Category name:"), juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor ("name", names[row]);
            aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton (T ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
            juce::Component::SafePointer<CategoryEditor> safe (this);
            aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, row] (int r)
            {
                if (safe != nullptr && r == 1 && row < safe->names.size())
                {
                    const auto t = aw->getTextEditorContents ("name").trim();
                    if (t.isNotEmpty()) safe->names.set (row, t);
                    safe->list.repaint();
                }
            }), true);
        }

        void move (int d)
        {
            const int r = list.getSelectedRow(), t = r + d;
            if (r < 0 || t < 0 || t >= names.size()) return;
            names.move (r, t);
            std::swap (origin[(size_t) r], origin[(size_t) t]);
            std::swap (colours[(size_t) r], colours[(size_t) t]);
            list.selectRow (t);
            list.repaint();
        }

        void pickColour (int row)
        {
            if (row < 0 || row >= (int) colours.size()) return;
            auto cs = std::make_unique<juce::ColourSelector> (juce::ColourSelector::showColourspace | juce::ColourSelector::showColourAtTop);
            cs->setCurrentColour (colours[(size_t) row]);
            cs->setSize (300, 280);
            juce::Component::SafePointer<CategoryEditor> safe (this);
            struct Listener : juce::ChangeListener
            {
                juce::Component::SafePointer<CategoryEditor> ed; int row;
                void changeListenerCallback (juce::ChangeBroadcaster* b) override
                {
                    if (ed == nullptr) return;
                    if (auto* sel = dynamic_cast<juce::ColourSelector*> (b))
                        if (row < (int) ed->colours.size()) { ed->colours[(size_t) row] = sel->getCurrentColour(); ed->list.repaint(); }
                }
            };
            listener = std::make_unique<Listener>();
            static_cast<Listener*> (listener.get())->ed = safe;
            static_cast<Listener*> (listener.get())->row = row;
            cs->addChangeListener (listener.get());
            juce::CallOutBox::launchAsynchronously (std::move (cs), kronosB.getScreenBounds(), nullptr);
        }

        void close()
        {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        }

        LiveSamplerProcessor& proc;
        juce::StringArray names;
        std::vector<int> origin;        // za svaki red: stari indeks (-1 = nova)
        std::vector<juce::Colour> colours;
        std::unique_ptr<juce::ChangeListener> listener;
        juce::ListBox list;
        juce::TextButton addB, renB, delB, upB, downB, kronosB, factoryB, okB, cancelB;
    };
}

void ListsPage::categoriesMenu()
{
    juce::PopupMenu m;
    if (proc.isKontrolerLinked())
    {
        m.addSectionHeader (T ("Linked with Live Kontroler (device Pikaso Workstation)"));
        m.addItem (99, proc.getKontrolerFile().getFullPathName(), false);
        m.addItem (98, proc.getKontrolerStatus(), false);
        m.addItem (4, T ("Edit categories..."));
        m.addItem (1, T ("Sync now"));
        m.addItem (2, T ("Choose another Live Kontroler file..."));
        m.addSeparator();
        m.addItem (3, T ("Unlink"));
    }
    else
    {
        m.addItem (4, T ("Edit categories..."));
        m.addSeparator();
        m.addItem (2, T ("Link with Live Kontroler (same categories and colours)..."));
    }
    juce::Component::SafePointer<ListsPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (categoriesButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& pr = safe->proc;
        auto show = [] (const juce::String& t) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("Live Kontroler"), t); };
        if (r == 1) { juce::String rep; pr.syncKontroler (&rep); show (rep); }
        if (r == 3) pr.linkKontroler ({});
        if (r == 4) safe->editCategories();
        if (r == 2)
        {
            auto start = pr.getKontrolerFile().existsAsFile() ? pr.getKontrolerFile() : LiveSamplerProcessor::defaultKontrolerFile();
            safe->chooser = std::make_unique<juce::FileChooser> (T ("Live Kontroler settings (podesavanja-server.json)"), start, "*.json");
            safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safe, show] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
                    safe->proc.linkKontroler (fc.getResult());
                    juce::String rep;
                    safe->proc.syncKontroler (&rep);
                    show (rep);
                });
        }
    });
}

void ListsPage::editCategories()
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new CategoryEditor (proc));
    o.dialogTitle = T ("Performance categories");
    o.dialogBackgroundColour = Pal::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

//==============================================================================
//  SetListPage
//==============================================================================
juce::Colour SetListPage::slotColour (int c)
{
    static const juce::Colour cols[] = { juce::Colour (0xff2a3140), juce::Colour (0xff8a3d1f), juce::Colour (0xff1f5e8a),
                                         juce::Colour (0xff2f7d4a), juce::Colour (0xff7a2a5a), juce::Colour (0xff7a6a1f),
                                         juce::Colour (0xff1f7a72), juce::Colour (0xff5a3d8a) };
    return cols[juce::jlimit (0, 7, c)];
}

SetListPage::SetListPage (LiveSamplerProcessor& p, LiveSamplerEditor& e) : proc (p), editor (e)
{
    listSelect.onChange = [this] { const int i = listSelect.getSelectedItemIndex(); if (i >= 0) proc.setCurrentSetList (i); };
    addAndMakeVisible (listSelect);

    nameEdit.setEditable (true);
    nameEdit.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    nameEdit.setColour (juce::Label::backgroundColourId, Pal::bg);
    nameEdit.setColour (juce::Label::textColourId, Pal::lcdText);
    nameEdit.setTooltip (T ("Set list name (double-click to edit)"));
    nameEdit.onTextChange = [this]
    {
        auto sl = proc.getSetList (proc.getCurrentSetList());
        sl.name = nameEdit.getText().trim();
        proc.updateSetList (proc.getCurrentSetList(), sl);
    };
    addAndMakeVisible (nameEdit);

    for (int i = 0; i < kSetListSlots / 16; ++i)
    {
        pageButtons[i].setButtonText (juce::String (i * 16 + 1) + "-" + juce::String (i * 16 + 16));
        pageButtons[i].setColour (juce::TextButton::buttonOnColourId, Pal::lcdText);
        pageButtons[i].getProperties().set ("fontSize", 12.0f);
        pageButtons[i].onClick = [this, i] { proc.setSetListPage (i); };
        addAndMakeVisible (pageButtons[i]);
    }
    editButton.setButtonText ("EDIT");
    editButton.setClickingTogglesState (true);
    editButton.setColour (juce::TextButton::buttonOnColourId, Pal::red);
    editButton.setTooltip (T ("Turn EDIT on, then click a button: set performance, label, colour"));
    editButton.onClick = [this] { repaint(); };
    addAndMakeVisible (editButton);

    pcMode.setButtonText (T ("Program Change selects set list"));
    pcMode.setTooltip (T ("On: CC0 selects the set list, Program Change 0-127 the button. Off: Program Change selects a performance from the list."));
    pcMode.onClick = [this] { proc.setProgramChangeMode (pcMode.getToggleState() ? 1 : 0); };
    addAndMakeVisible (pcMode);
}

void SetListPage::refresh()
{
    listSelect.clear (juce::dontSendNotification);
    for (int i = 0; i < kNumSetLists; ++i)
    {
        const auto& n = proc.getSetList (i).name;
        listSelect.addItem (juce::String (i + 1) + "  " + (n.isNotEmpty() ? n : T ("Set list")), i + 1);
    }
    listSelect.setSelectedItemIndex (proc.getCurrentSetList(), juce::dontSendNotification);
    if (! nameEdit.isBeingEdited())
    {
        const auto& n = proc.getSetList (proc.getCurrentSetList()).name;
        nameEdit.setText (n.isNotEmpty() ? n : T ("Set list ") + juce::String (proc.getCurrentSetList() + 1), juce::dontSendNotification);
    }
    for (int i = 0; i < kSetListSlots / 16; ++i)
        pageButtons[i].setToggleState (i == proc.getSetListPage(), juce::dontSendNotification);
    pcMode.setToggleState (proc.getProgramChangeMode() == 1, juce::dontSendNotification);
    repaint();
}

juce::Rectangle<int> SetListPage::gridArea() const
{
    auto r = getLocalBounds().reduced (12, 10);
    r.removeFromTop (36 + 8 + 30 + 10);
    return r;
}

int SetListPage::cellAt (juce::Point<int> p) const
{
    auto g = gridArea();
    if (! g.contains (p)) return -1;
    const int c = (p.x - g.getX()) * 4 / g.getWidth();
    const int r = (p.y - g.getY()) * 4 / g.getHeight();
    return proc.getSetListPage() * 16 + juce::jlimit (0, 15, r * 4 + c);
}

void SetListPage::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    auto area = gridArea().toFloat();
    const float cw = area.getWidth() / 4.0f, ch = area.getHeight() / 4.0f;
    const auto& sl = proc.getSetList (proc.getCurrentSetList());
    const bool editing = editButton.getToggleState();
    for (int k = 0; k < 16; ++k)
    {
        const int slot = proc.getSetListPage() * 16 + k;
        const auto& s = sl.slots[(size_t) slot];
        auto cell = juce::Rectangle<float> (area.getX() + (k % 4) * cw, area.getY() + (k / 4) * ch, cw, ch).reduced (5.0f);
        const bool has = s.perf >= 0 && proc.getSlot (s.perf).used;
        const bool current = proc.getCurrentSetListSlot() == slot;
        auto col = has ? slotColour (s.colour) : Pal::bg;
        if (current) col = col.brighter (0.6f);
        g.setColour (col);
        g.fillRoundedRectangle (cell, 10.0f);
        g.setColour (current ? Pal::lcdText : (editing ? Pal::red.withAlpha (0.7f) : Pal::panelHi));
        g.drawRoundedRectangle (cell, 10.0f, current ? 3.0f : 1.5f);

        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (juce::String (slot + 1), cell.reduced (12.0f, 8.0f), juce::Justification::topLeft);
        if (! has) continue;
        const auto& perfName = proc.getSlot (s.perf).name;
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (juce::jmin (30.0f, ch * 0.24f), juce::Font::bold));
        g.drawFittedText (s.label.isNotEmpty() ? s.label : perfName, cell.reduced (12.0f, 24.0f).toNearestInt(), juce::Justification::centred, 2);
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (proc.getBankName (s.perf / kPerfPerBank) + " " + juce::String (s.perf % kPerfPerBank).paddedLeft ('0', 3)
                        + (s.label.isNotEmpty() ? "  " + perfName : juce::String()),
                    cell.reduced (12.0f, 8.0f), juce::Justification::bottomLeft, true);
    }
}

void SetListPage::mouseDown (const juce::MouseEvent& e)
{
    const int slot = cellAt (e.getPosition());
    if (slot < 0) return;
    if (editButton.getToggleState() || e.mods.isPopupMenu())
        editSlot (slot);
    else
        proc.recallSetListSlot (slot);
}

void SetListPage::editSlot (int slot)
{
    juce::PopupMenu m, pick, colours;
    m.addItem (1, T ("Put the current performance"), proc.getCurrentSlot() >= 0);
    for (int b = 0; b < kNumPerfBanks; ++b)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < kPerfPerBank; ++i)
            if (const auto& s = proc.getSlot (b * kPerfPerBank + i); s.used)
                sub.addItem (1000 + b * kPerfPerBank + i, juce::String (i).paddedLeft ('0', 3) + "  " + s.name);
        if (sub.getNumItems() > 0)
            pick.addSubMenu (proc.getBankName (b), sub);
    }
    m.addSubMenu (T ("Choose performance"), pick);
    m.addItem (2, T ("Button label..."));
    const char* cn[] = { "Grey", "Orange", "Blue", "Green", "Purple", "Yellow", "Turquoise", "Indigo" };
    for (int c = 0; c < 8; ++c)
        colours.addColouredItem (100 + c, T (cn[c]), slotColour (c).brighter (0.4f));
    m.addSubMenu (T ("Colour"), colours);
    m.addSeparator();
    m.addItem (3, T ("Clear button"));

    juce::Component::SafePointer<SetListPage> safe (this);
    m.showMenuAsync ({}, [safe, slot] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& pr = safe->proc;
        auto sl = pr.getSetList (pr.getCurrentSetList());
        auto& s = sl.slots[(size_t) slot];
        if (r == 1) s.perf = pr.getCurrentSlot();
        if (r >= 1000) s.perf = r - 1000;
        if (r >= 100 && r < 108) s.colour = r - 100;
        if (r == 3) s = SetListSlot();
        if (r == 2)
        {
            auto* w = new juce::AlertWindow (T ("Label"), T ("Button text (empty = performance name)"), juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("t", s.label);
            w->addButton ("OK", 1);
            w->addButton (T ("Cancel"), 0);
            w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, slot, w] (int res)
            {
                if (safe == nullptr || res == 0) return;
                auto& p2 = safe->proc;
                auto sl2 = p2.getSetList (p2.getCurrentSetList());
                sl2.slots[(size_t) slot].label = w->getTextEditorContents ("t").trim();
                p2.updateSetList (p2.getCurrentSetList(), sl2);
            }), true);
            return;
        }
        pr.updateSetList (pr.getCurrentSetList(), sl);
    });
}

void SetListPage::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto top = r.removeFromTop (36);
    listSelect.setBounds (top.removeFromLeft (260).reduced (0, 3));
    top.removeFromLeft (10);
    nameEdit.setBounds (top.removeFromLeft (300).reduced (0, 3));
    editButton.setBounds (top.removeFromRight (100));
    top.removeFromRight (10);
    pcMode.setBounds (top.removeFromRight (260));
    r.removeFromTop (8);
    auto pages = r.removeFromTop (30);
    const int pw = pages.getWidth() / (kSetListSlots / 16);
    for (auto& b : pageButtons)
        b.setBounds (pages.removeFromLeft (pw).reduced (2, 0));
}

#include "Pages.h"
#include "Text.h"
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
//  PartStrip
//==============================================================================
PartStrip::PartStrip (LiveSamplerProcessor& p, LiveSamplerEditor& e, int i)
    : proc (p), editor (e), index (i), colour (Pal::part[i]),
      volume (T ("Jačina"), Pal::part[i], -60.0, 6.0, -12.0, UI::dbText, 0.0),
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
    addAndMakeVisible (userButton);

    editButton.setButtonText ("EDIT USER");
    editButton.getProperties().set ("fontSize", 11.0f);
    editButton.onClick = [this] { editor.editUser (proc.getPerformance().parts[(size_t) index].user); };
    addAndMakeVisible (editButton);

    partMenu.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
    partMenu.setTooltip (T ("Kopiraj / nalepi ceo deo (user, jačina, EQ, split...) u drugi performans"));
    partMenu.onClick = [this] { showPartMenu(); };
    addAndMakeVisible (partMenu);

    eqButton.setButtonText ("EQ");
    eqButton.getProperties().set ("fontSize", 11.0f);
    eqButton.setTooltip (T ("Doteraj EQ ovog dela (dodaje se na EQ usera)"));
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
    UI::setupIncDec (channel, 0, 16, 1, [] (double v) { return v < 0.5 ? T ("Svi") : juce::String ((int) v); });

    transpose.onValueChange = [this] { edit ([this] (PerfPart& pp) { pp.transpose = (int) transpose.getValue(); }); };
    octave.onValueChange    = [this] { edit ([this] (PerfPart& pp) { pp.octave = (int) octave.getValue(); }); };
    keyLow.onValueChange    = [this] { edit ([this] (PerfPart& pp) { pp.keyLow = (int) keyLow.getValue(); pp.keyHigh = juce::jmax (pp.keyHigh, pp.keyLow); }); };
    keyHigh.onValueChange   = [this] { edit ([this] (PerfPart& pp) { pp.keyHigh = (int) keyHigh.getValue(); pp.keyLow = juce::jmin (pp.keyLow, pp.keyHigh); }); };
    velLow.onValueChange    = [this] { edit ([this] (PerfPart& pp) { pp.velLow = (int) velLow.getValue(); pp.velHigh = juce::jmax (pp.velHigh, pp.velLow); }); };
    velHigh.onValueChange   = [this] { edit ([this] (PerfPart& pp) { pp.velHigh = (int) velHigh.getValue(); pp.velLow = juce::jmin (pp.velLow, pp.velHigh); }); };
    channel.onValueChange   = [this] { edit ([this] (PerfPart& pp) { pp.midiChannel = (int) channel.getValue(); }); };

    const char* names[] = { "Transp", "Oktava", "Od", "Do", "Vel od", "Vel do", "Kanal" };
    juce::Slider* boxes[] = { &transpose, &octave, &keyLow, &keyHigh, &velLow, &velHigh, &channel };
    for (int k = 0; k < 7; ++k)
    {
        styleLabel (labels[k], T (names[k]), 11.5f);
        addAndMakeVisible (labels[k]);
        addAndMakeVisible (boxes[k]);
    }

    tercaButton.setButtonText ("TERCA");
    tercaButton.setClickingTogglesState (true);
    tercaButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3ddc84));
    tercaButton.getProperties().set ("fontSize", 12.0f);
    tercaButton.setTooltip (T ("Ovaj deo dobija terce kad su uključene"));
    tercaButton.onClick = [this] { edit ([this] (PerfPart& pp) { pp.terca = tercaButton.getToggleState(); }); };
    addAndMakeVisible (tercaButton);

    sustainButton.setButtonText ("SUST");
    sustainButton.setClickingTogglesState (true);
    sustainButton.setColour (juce::TextButton::buttonOnColourId, Pal::dim);
    sustainButton.getProperties().set ("fontSize", 12.0f);
    sustainButton.setTooltip (T ("Sustain pedala utiče na ovaj deo"));
    sustainButton.onClick = [this] { edit ([this] (PerfPart& pp) { pp.sustainPedal = sustainButton.getToggleState(); }); };
    addAndMakeVisible (sustainButton);

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
                info.setText (T ("EQ usera: bas ") + db (us.eqLowDb) + T (", sredina ") + db (us.eqMidDb)
                              + " @ " + juce::String (juce::roundToInt (us.eqMidFreq)) + T (" Hz, visoki ") + db (us.eqHighDb)
                              + T ("\nOvde podešavaš samo za ovaj performans."), juce::dontSendNotification);
            }
            info.setFont (juce::FontOptions (12.0f));
            info.setColour (juce::Label::textColourId, Pal::dim);
            addAndMakeVisible (info);

            const char* names[] = { "Bas", "Sredina", "Visoki" };
            const float vals[] = { part.eqLowDb, part.eqMidDb, part.eqHighDb };
            for (int k = 0; k < 3; ++k)
            {
                auto* kn = knobs.add (new ValueKnob (T (names[k]), c, -15.0, 15.0, 0.0, db, 0.0));
                kn->setValue (vals[k]);
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

void PartStrip::showPartMenu()
{
    juce::PopupMenu m;
    m.addItem (1, T ("Kopiraj deo"));
    m.addItem (2, T ("Nalepi deo"), Clip::part.has_value());
    m.addSeparator();
    m.addItem (3, T ("Isprazni deo"));
    juce::Component::SafePointer<PartStrip> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&partMenu), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
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

void PartStrip::showUserMenu()
{
    const auto& part = proc.getPerformance().parts[(size_t) index];
    juce::PopupMenu menu;
    const auto names = proc.getUserNames();
    for (int i = 0; i < names.size(); ++i)
        menu.addItem (i + 1, names[i], true, names[i] == part.user);
    menu.addSeparator();
    menu.addItem (90001, T ("+ Novi user od instrumenta..."));
    menu.addItem (90002, T ("+ Prazan novi user"));
    menu.addItem (90003, T ("(bez usera)"));

    juce::Component::SafePointer<PartStrip> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&userButton).withMinimumWidth (240),
        [safe, names] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& self = *safe;
            if (r == 90001)
            {
                UI::showInstrumentMenu (&self.userButton, self.proc.getInstrumentList(), {}, false,
                    [safe] (const juce::String& inst)
                    {
                        if (safe == nullptr || inst.isEmpty()) return;
                        const int u = safe->proc.createUserFromInstrument (inst);
                        const auto name = safe->proc.getUser (u).name;
                        safe->edit ([name] (PerfPart& pp) { pp.user = name; pp.on = true; });
                    });
            }
            else if (r == 90002)
            {
                const int u = self.proc.createUser ("User");
                const auto name = self.proc.getUser (u).name;
                self.edit ([name] (PerfPart& pp) { pp.user = name; pp.on = true; });
                self.editor.editUser (name);
            }
            else if (r == 90003)
            {
                self.edit ([] (PerfPart& pp) { pp.user = {}; pp.on = false; });
            }
            else if (r - 1 < names.size())
            {
                const auto name = names[r - 1];
                self.edit ([name] (PerfPart& pp) { pp.user = name; pp.on = true; });
            }
        });
}

void PartStrip::refresh()
{
    const auto& p = proc.getPerformance().parts[(size_t) index];
    onButton.setToggleState (p.on, juce::dontSendNotification);
    userButton.setButtonText (p.user.isEmpty() ? T ("— izaberi usera —") : p.user);
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
    setAlpha (p.on ? 1.0f : 0.55f);
}

void PartStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (colour);
    g.fillRoundedRectangle (r.removeFromTop (4.0f).reduced (8.0f, 0.0f), 2.0f);
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
    userButton.setBounds (r.removeFromTop (40));
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
        auto knobs1 = r.removeFromTop (60);
        volume.setBounds (knobs1.removeFromLeft (knobs1.getWidth() / 2));
        pan.setBounds (knobs1);
        auto knobs2 = r.removeFromTop (60);
        reverb.setBounds (knobs2.removeFromLeft (knobs2.getWidth() / 2));
        delay.setBounds (knobs2);
    }
    r.removeFromTop (4);

    juce::Slider* boxes[] = { &transpose, &octave, &keyLow, &keyHigh, &velLow, &velHigh, &channel };
    for (int k = 0; k < 7; ++k)
    {
        auto row = r.removeFromTop (compact ? 22 : 25);
        labels[k].setBounds (row.removeFromLeft (compact ? 34 : 46));
        boxes[k]->setBounds (row.reduced (0, 1));
        boxes[k]->setTextBoxStyle (juce::Slider::TextBoxLeft, false, juce::jmax (40, row.getWidth() - 40), 22);
    }
    r.removeFromTop (4);
    auto toggles = r.removeFromTop (24);
    tercaButton.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2).reduced (1, 0));
    sustainButton.setBounds (toggles.reduced (1, 0));
}

//==============================================================================
//  KeyRangeView
//==============================================================================
void KeyRangeView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r, 8.0f);

    auto area = r.reduced (10.0f, 6.0f);
    auto keys = area.removeFromBottom (16.0f);
    area.removeFromBottom (3.0f);
    const int lo = 21, hi = 108;
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
    juce::Array<int> active;
    for (int i = 0; i < kNumParts; ++i)
        if (perf.parts[(size_t) i].on && perf.parts[(size_t) i].user.isNotEmpty())
            active.add (i);

    if (active.isEmpty())
    {
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (T ("Nijedan deo nije uključen"), area, juce::Justification::centred);
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
        g.setColour (Pal::part[i].withAlpha (0.85f));
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (juce::Colours::black);
        juce::String txt = juce::String (i + 1) + "  " + p.user;
        if (p.transpose != 0 || p.octave != 0)
            txt << "  (" << (p.octave * 12 + p.transpose > 0 ? "+" : "") << (p.octave * 12 + p.transpose) << ")";
        g.drawText (txt, bar.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
    }
}

//==============================================================================
//  PerformancePage
//==============================================================================
PerformancePage::PerformancePage (LiveSamplerProcessor& p, LiveSamplerEditor& e)
    : proc (p), ranges (p)
{
    styleLabel (nameLabel, T ("IME PERFORMANSA"), 12.5f);
    addAndMakeVisible (nameLabel);

    nameEdit.setEditable (true);
    nameEdit.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    nameEdit.setColour (juce::Label::backgroundColourId, Pal::bg);
    nameEdit.setColour (juce::Label::textColourId, Pal::lcdText);
    nameEdit.onTextChange = [this]
    {
        auto perf = proc.getPerformance();
        perf.name = nameEdit.getText().trim().isEmpty() ? juce::String ("Performans") : nameEdit.getText().trim();
        proc.updatePerformance (perf);
    };
    addAndMakeVisible (nameEdit);

    for (int i = 0; i < kNumParts; ++i)
        addChildComponent (strips.add (new PartStrip (p, e, i)));
    addAndMakeVisible (ranges);

    const char* pageNames[] = { "DELOVI 1-8", "DELOVI 9-16" };
    for (int b = 0; b < 2; ++b)
    {
        pageButtons[b].setButtonText (pageNames[b]);
        pageButtons[b].setClickingTogglesState (false);
        pageButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        pageButtons[b].onClick = [this, b] { page = b; resized(); refresh(); };
        addAndMakeVisible (pageButtons[b]);
    }
}

void PerformancePage::refresh()
{
    if (! nameEdit.isBeingEdited())
        nameEdit.setText (proc.getPerformance().name, juce::dontSendNotification);
    for (auto* s : strips)
        s->refresh();
    for (int b = 0; b < 2; ++b)
    {
        // oznaci stranu na kojoj ima ukljucenih delova
        bool any = false;
        for (int i = b * 8; i < b * 8 + 8; ++i)
            any = any || (proc.getPerformance().parts[(size_t) i].on && proc.getPerformance().parts[(size_t) i].user.isNotEmpty());
        pageButtons[b].setToggleState (b == page, juce::dontSendNotification);
        pageButtons[b].setButtonText (juce::String (b == 0 ? "DELOVI 1-8" : "DELOVI 9-16") + (any ? " ●" : ""));
    }
    ranges.repaint();
}

void PerformancePage::paint (juce::Graphics&) {}

void PerformancePage::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (32);
    nameLabel.setBounds (top.removeFromLeft (140));
    nameEdit.setBounds (top.removeFromLeft (360).reduced (0, 2));
    pageButtons[1].setBounds (top.removeFromRight (130).reduced (2, 2));
    pageButtons[0].setBounds (top.removeFromRight (130).reduced (2, 2));

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
    instButton.onClick = [this]
    {
        const auto& u = proc.getUser (userIndex());
        juce::Component::SafePointer<OscRow> safe (this);
        UI::showInstrumentMenu (&instButton, proc.getInstrumentList(), u.osc[(size_t) oscIndex()].instrument, true,
            [safe] (const juce::String& name)
            {
                if (safe == nullptr) return;
                safe->edit ([name] (OscSettings& s) { s.instrument = name; s.on = name.isNotEmpty(); });
            });
    };
    addAndMakeVisible (instButton);

    mapButton.setButtonText ("MAPA");
    mapButton.getProperties().set ("fontSize", 11.0f);
    mapButton.setTooltip (T ("Otvori editor mapiranja za ovaj instrument"));
    mapButton.onClick = [this] { editor.editMapping (proc.getUser (userIndex()).name, oscIndex()); };
    addAndMakeVisible (mapButton);

    menuButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
    menuButton.setTooltip (T ("Kopiraj / nalepi / obriši ovaj MS"));
    menuButton.onClick = [this] { showMenu(); };
    addAndMakeVisible (menuButton);

    keyOffButton.setButtonText ("KO");
    keyOffButton.setClickingTogglesState (true);
    keyOffButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    keyOffButton.getProperties().set ("fontSize", 11.0f);
    keyOffButton.setTooltip (T ("Key off: ovaj MS svira kad pustiš taster (zvuk puštanja žice, klik i sl.)"));
    keyOffButton.onClick = [this] { edit ([this] (OscSettings& s) { s.keyOff = keyOffButton.getToggleState(); }); };
    addAndMakeVisible (keyOffButton);

    eqButton.setButtonText ("EQ");
    eqButton.setClickingTogglesState (false);
    eqButton.getProperties().set ("fontSize", 11.0f);
    eqButton.setTooltip (T ("EQ samo za ovaj MS"));
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
}

void OscRow::showMenu()
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    juce::PopupMenu m;
    m.addItem (1, T ("Kopiraj MS"));
    m.addItem (2, T ("Nalepi MS"), Clip::ms.has_value());
    m.addSeparator();
    m.addItem (3, T ("Obriši MS"));
    {
        const auto& cur = proc.getUser (u).osc[(size_t) oscIndex()];
        const auto layers = cur.on ? proc.velocityLayersOf (cur.instrument) : std::vector<int>();
        if (layers.size() > 1)
        {
            m.addSeparator();
            m.addSectionHeader (T ("Velocity slojevi (") + juce::String ((int) layers.size()) + ")");
            m.addItem (4, T ("Razloži slojeve na MS1-MS") + juce::String ((int) layers.size()));
            m.addItem (5, T ("Obriši ovaj velocity sloj sa diska (") + juce::String (cur.velLow) + "-" + juce::String (cur.velHigh) + ")...");
        }
    }
    juce::Component::SafePointer<OscRow> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto info = [] (const juce::String& t) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("Velocity slojevi"), t); };
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
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("Obriši velocity sloj"),
                T ("Trajno brišem sa diska sve semplove ovog sloja (jačina ") + juce::String (o.velLow) + "-" + juce::String (o.velHigh)
                    + T (") za:\n") + o.instrument + T ("\n\nSusedni sloj preuzima njegov opseg. Ovo važi za sve usere koji koriste ovaj zvuk."),
                T ("Obriši"), T ("Otkaži"), nullptr,
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
            const K ks[] = { { "Bas", -15, 15, 0, 0, s.eqLowDb, db }, { "Sredina", -15, 15, 0, 0, s.eqMidDb, db },
                             { "Sredina Hz", 100, 8000, 1000, 1000, s.eqMidFreq, UI::hzText }, { "Visoki", -15, 15, 0, 0, s.eqHighDb, db } };
            for (int k = 0; k < 4; ++k)
            {
                auto* kn = knobs.add (new ValueKnob (T (ks[k].n), c, ks[k].lo, ks[k].hi, ks[k].centre, ks[k].f, ks[k].def));
                kn->setValue (ks[k].val);
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
        instText = T ("⚠ nema: ") + s.instrument;
    instButton.setButtonText (instText);
    mapButton.setEnabled (s.instrument.isNotEmpty());
    onButton.setColour (juce::TextButton::buttonOnColourId, Pal::part[blockFn()]);
    keyOffButton.setToggleState (s.keyOff, juce::dontSendNotification);
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
    instButton.setBounds (instArea);
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
static juce::String octText (double v)    { return (v > 0 ? "+" : "") + juce::String (v, 1) + " okt"; }
static juce::String eqDb (double v)       { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; }
static juce::String hzText2 (double v)    { return juce::String (v, 2) + " Hz"; }
static juce::String stText (double v)     { return (v > 0 ? "+" : "") + juce::String (v, 1) + " st"; }

SoundEditor::SoundEditor (LiveSamplerProcessor& p, std::function<int()> ui)
    : proc (p), userIndex (std::move (ui))
{
    for (int b = 0; b < kNumBlocks; ++b)
    {
        blockButtons[b].setButtonText ("OSC" + juce::String (b + 1));
        blockButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::part[b]);
        blockButtons[b].getProperties().set ("fontSize", 12.0f);
        blockButtons[b].setTooltip (T ("Izaberi OSC: gore se vide njegovi MS1-MS8, dole njegov zvuk. Kronos koristi OSC1 i OSC2."));
        blockButtons[b].onClick = [this, b] { block = b; rebuildKnobs(); if (onBlockChanged) onBlockChanged(); };
        addAndMakeVisible (blockButtons[b]);
    }
    blockMenu.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
    blockMenu.setTooltip (T ("Kopiraj / nalepi / obriši ceo OSC (zvuk i svih 8 MS)"));
    blockMenu.onClick = [this] { showBlockMenu(); };
    addAndMakeVisible (blockMenu);

    // MONO / PRELAZ
    auto styleL = [] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, Pal::dim);
    };
    styleL (monoLabel, T ("MONO OPSEG (van opsega: poly, bez portamenta)"));
    styleL (articLabel, T ("PRELAZ: na koje intervale (polustepene) se pali ovaj OSC"));
    articHelp.setText (T ("Uključi PRELAZ na više OSC-ova. OSC bez izabranih intervala svira prvi ton. Dok držiš ton i odsviraš "
                          "sledeći, OSC čiji je interval pogođen se pali, a prethodni se gasi (kao prelaz žice preko pragova). "
                          "Radi kad je user Mono / Mono legato."), juce::dontSendNotification);
    articHelp.setFont (juce::FontOptions (12.0f));
    articHelp.setColour (juce::Label::textColourId, Pal::dim);
    articHelp.setJustificationType (juce::Justification::topLeft);
    auto c3 = [this] { return proc.getC3Is60(); };
    UI::setupNoteBox (monoLow, c3);
    UI::setupNoteBox (monoHigh, c3);
    monoLow.onValueChange = [this] { const int v = (int) monoLow.getValue(); const int b = block; edit ([v, b] (UserProgram& u) { auto& s = u.block[(size_t) b]; s.monoLow = v; s.monoHigh = juce::jmax (s.monoHigh, v); }); };
    monoHigh.onValueChange = [this] { const int v = (int) monoHigh.getValue(); const int b = block; edit ([v, b] (UserProgram& u) { auto& s = u.block[(size_t) b]; s.monoHigh = v; s.monoLow = juce::jmin (s.monoLow, v); }); };
    articOn.setButtonText (T ("PRELAZ"));
    articOn.setClickingTogglesState (true);
    articOn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    articOn.onClick = [this] { const bool v = articOn.getToggleState(); const int b = block; edit ([v, b] (UserProgram& u) { u.block[(size_t) b].articOn = v; }); };
    articDir.addItemList ({ T ("Gore i dole"), T ("Samo gore"), T ("Samo dole") }, 1);
    articDir.onChange = [this] { const int v = articDir.getSelectedItemIndex(); const int b = block; edit ([v, b] (UserProgram& u) { u.block[(size_t) b].articDir = v; }); };
    const char* iv[] = { "mala sekunda", "velika sekunda", "mala terca", "velika terca", "kvarta", "triton", "kvinta",
                         "mala seksta", "velika seksta", "mala septima", "velika septima", "oktava" };
    for (int i = 0; i < 24; ++i)
    {
        auto& b = intervalButtons[i];
        b.setButtonText (juce::String (i + 1));
        b.setTooltip (juce::String (i + 1) + T (" polustepen(a)") + (i < 12 ? juce::String (" - ") + T (iv[i]) : (i == 23 ? T (" - dve oktave") : T (" - preko oktave"))));
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

    const char* secNames[] = { "PITCH", "FILTER", "AMP", "MONO / PRELAZ", "ZAJEDNIČKO", "EFEKAT / EQ" };
    for (int s = 0; s < NumSections; ++s)
    {
        sectionButtons[s].setButtonText (T (secNames[s]));
        sectionButtons[s].setColour (juce::TextButton::buttonOnColourId, Pal::lcdText);
        sectionButtons[s].onClick = [this, s] { showSection (s); };
        addAndMakeVisible (sectionButtons[s]);
    }
    addAndMakeVisible (envView);

    filterType.addItemList ({ T ("Isključen"), "Lowpass 24 dB", "Lowpass 12 dB", "Highpass", "Bandpass" }, 1);
    filterType.onChange = [this] { const int t = filterType.getSelectedItemIndex(); const int b = block; edit ([t, b] (UserProgram& u) { u.block[(size_t) b].filterType = t; }); };
    filterLabel.setText ("TIP FILTERA", juce::dontSendNotification);
    filterLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    filterLabel.setColour (juce::Label::textColourId, Pal::dim);
    addChildComponent (filterType);
    addChildComponent (filterLabel);

    voiceMode.addItemList ({ "Poly", "Mono", "Mono legato" }, 1);
    voiceMode.onChange = [this] { const int v = voiceMode.getSelectedItemIndex(); edit ([v] (UserProgram& u) { u.voiceMode = v; }); };
    priority.addItemList ({ T ("Poslednja nota"), T ("Najviša nota"), T ("Najniža nota") }, 1);
    priority.onChange = [this] { const int v = priority.getSelectedItemIndex(); edit ([v] (UserProgram& u) { u.priority = v; }); };
    for (auto* b : { &portaOn, &portaFingered, &portaConstRate })
    {
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        b->getProperties().set ("fontSize", 13.0f);
    }
    portaOn.setButtonText ("PORTAMENTO");
    portaFingered.setButtonText ("Samo vezano (fingered)");
    portaConstRate.setButtonText ("Konstantna brzina");
    portaConstRate.setTooltip (T ("Veći skok traje duže (vreme je po oktavi)"));
    portaOn.onClick = [this] { const bool v = portaOn.getToggleState(); edit ([v] (UserProgram& u) { u.portaOn = v; }); };
    portaFingered.onClick = [this] { const bool v = portaFingered.getToggleState(); edit ([v] (UserProgram& u) { u.portaFingered = v; }); };
    portaConstRate.onClick = [this] { const bool v = portaConstRate.getToggleState(); edit ([v] (UserProgram& u) { u.portaConstRate = v; }); };
    portaTime = std::make_unique<ValueKnob> ("Vreme", Pal::accent, 0.001, 3.0, 0.3, UI::timeText, 0.1);
    portaTime->onChange = [this] (double v) { edit ([v] (UserProgram& u) { u.portamento = (float) v; }); };
    UI::setupIncDec (bendUp, 0, 24, 1);
    UI::setupIncDec (bendDown, 0, 24, 1);
    bendUp.onValueChange = [this] { const int v = (int) bendUp.getValue(); edit ([v] (UserProgram& u) { u.bendUp = v; }); };
    bendDown.onValueChange = [this] { const int v = (int) bendDown.getValue(); edit ([v] (UserProgram& u) { u.bendDown = v; }); };
    const char* cl[] = { "GLAS", "PRIORITET", "BEND GORE", "BEND DOLE" };
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
    fxLabel.setText ("INSERT EFEKAT", juce::dontSendNotification);
    fxLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    fxLabel.setColour (juce::Label::textColourId, Pal::dim);
    addChildComponent (fxType);
    addChildComponent (fxLabel);

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

void SoundEditor::rebuildKnobs()
{
    knobs.clear();
    rows.clear();
    const auto acc = Pal::part[block];

    auto P = [] (const char* n, double lo, double hi, double c, double d, std::function<juce::String (double)> f,
                 std::function<float& (UserProgram&, int)> r) { return Param { T (n), lo, hi, c, d, std::move (f), std::move (r) }; };
    using U = UserProgram;

    switch (section)
    {
        case Pitch:
            rows.push_back ({
                P ("EG intenz.", -12, 12, 0, 0, stText, [] (U& u, int b) -> float& { return u.block[(size_t) b].pitchEgInt; }),
                P ("Vibrato brz.", 0.1, 12, 4, 5.5, hzText2, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibRate; }),
                P ("Vibrato dub.", 0, 300, 50, 0, centsText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibDepth; }),
                P ("Kašnjenje", 0, 5, 0.5, 0.3, UI::timeText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibDelay; }),
                P ("Fade", 0, 5, 0.5, 0.25, UI::timeText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibFade; }),
                P ("Offset", -1, 1, 0, 0, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].vibOffset; }),
                P ("Točkić→vib", 0, 300, 50, 30, centsText, [] (U& u, int b) -> float& { return u.block[(size_t) b].mwToVib; }),
                P ("Aftert.→vib", 0, 100, 0, 0, centsText, [] (U& u, int b) -> float& { return u.block[(size_t) b].atToVib; }) });
            rows.push_back (envParams ([] (U& u, int b) -> EnvSettings& { return u.block[(size_t) b].pitchEg; }, true, true));
            break;

        case Filter:
            rows.push_back ({
                P ("Cutoff", 40, 20000, 1500, 20000, UI::hzText, [] (U& u, int b) -> float& { return u.block[(size_t) b].cutoff; }),
                P ("Rezonanca", 0, 1, 0, 0, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].resonance; }),
                P ("Key track", 0, 2, 0, 0, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].keyTrack; }),
                P ("Velocity", -4, 4, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].velToCutoff; }),
                P ("EG intenz.", -10, 10, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].filterEgInt; }),
                P ("Vel→EG", -10, 10, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].velToFilterEg; }),
                P ("Bend→cut", -6, 6, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].bendToCutoff; }),
                P ("LFO brzina", 0.05, 15, 3, 2, hzText2, [] (U& u, int b) -> float& { return u.block[(size_t) b].filterLfoRate; }),
                P ("LFO dubina", 0, 3, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].filterLfoDepth; }),
                P ("Točkić→cut", -4, 4, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].mwToCutoff; }),
                P ("Aftert.→cut", -4, 4, 0, 0, octText, [] (U& u, int b) -> float& { return u.block[(size_t) b].atToCutoff; }) });
            rows.push_back (envParams ([] (U& u, int b) -> EnvSettings& { return u.block[(size_t) b].filterEg; }, true, true));
            break;

        case Amp:
            rows.push_back ({
                P ("Velocity", 0, 1, 0, 0.8, UI::pctText, [] (U& u, int b) -> float& { return u.block[(size_t) b].velSens; }),
                P ("Vel. kriva", 0.3, 4, 1.4, 1.6, [] (double v) { return juce::String (v, 2); }, [] (U& u, int b) -> float& { return u.block[(size_t) b].velCurve; }),
                P ("Aftert.→jač.", 0, 12, 0, 0, [] (double v) { return "+" + juce::String (v, 1) + " dB"; }, [] (U& u, int b) -> float& { return u.block[(size_t) b].atToAmp; }) });
            rows.push_back (envParams ([] (U& u, int b) -> EnvSettings& { return u.block[(size_t) b].ampEg; }, false, false));
            break;

        case Effect:
            rows.push_back ({
                P ("FX brzina", 0, 1, 0, 0.3, UI::pctText, [] (U& u, int) -> float& { return u.fxRate; }),
                P ("FX dubina", 0, 1, 0, 0.5, UI::pctText, [] (U& u, int) -> float& { return u.fxDepth; }),
                P ("FX mix", 0, 1, 0, 0.5, UI::pctText, [] (U& u, int) -> float& { return u.fxMix; }),
                P ("Send reverb", 0, 1, 0, 1, UI::pctText, [] (U& u, int) -> float& { return u.send1; }),
                P ("Send delay", 0, 1, 0, 1, UI::pctText, [] (U& u, int) -> float& { return u.send2; }),
                P ("EQ bas", -15, 15, 0, 0, eqDb, [] (U& u, int) -> float& { return u.eqLowDb; }),
                P ("EQ sredina", -15, 15, 0, 0, eqDb, [] (U& u, int) -> float& { return u.eqMidDb; }),
                P ("Sredina Hz", 100, 8000, 1000, 1000, UI::hzText, [] (U& u, int) -> float& { return u.eqMidFreq; }),
                P ("EQ visoki", -15, 15, 0, 0, eqDb, [] (U& u, int) -> float& { return u.eqHighDb; }) });
            break;

        default:
            break;
    }

    for (auto& row : rows)
        for (auto& prm : row)
        {
            auto* k = knobs.add (new ValueKnob (prm.name, acc, prm.lo, prm.hi, prm.centre, prm.fmt, prm.def));
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
    lvl ("Attack niv.", 1.0, &EnvSettings::attackLevel);
    tim ("Decay", 0.3, &EnvSettings::decayTime);
    lvl ("Break niv.", 1.0, &EnvSettings::breakLevel);
    tim ("Slope", 0.3, &EnvSettings::slopeTime);
    lvl ("Sustain", 1.0, &EnvSettings::sustain);
    tim ("Release", 0.3, &EnvSettings::releaseTime);
    if (withReleaseLevel)
        lvl ("Release niv.", 0.0, &EnvSettings::releaseLevel);
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
    voiceMode.setSelectedItemIndex (u.voiceMode, juce::dontSendNotification);
    priority.setSelectedItemIndex (u.priority, juce::dontSendNotification);
    portaOn.setToggleState (u.portaOn, juce::dontSendNotification);
    portaFingered.setToggleState (u.portaFingered, juce::dontSendNotification);
    portaConstRate.setToggleState (u.portaConstRate, juce::dontSendNotification);
    portaTime->setValue (u.portamento);
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
    }
}

void SoundEditor::showBlockMenu()
{
    const int ui = userIndex();
    if (ui < 0 || ui >= proc.getNumUsers()) return;
    juce::PopupMenu m;
    m.addItem (1, T ("Kopiraj OSC") + juce::String (block + 1));
    m.addItem (2, T ("Nalepi u OSC") + juce::String (block + 1), Clip::osc.has_value());
    m.addSeparator();
    m.addItem (3, T ("Obriši OSC") + juce::String (block + 1));
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
    top.removeFromLeft (10);
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
//  UserPage
//==============================================================================
UserPage::UserPage (LiveSamplerProcessor& p, LiveSamplerEditor& e)
    : proc (p), editor (e), sound (p, [this] { return userIndex; })
{
    list.setModel (this);
    list.setRowHeight (26);
    list.setColour (juce::ListBox::outlineColourId, Pal::panelHi);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    newButton.setButtonText ("Novi");
    newButton.onClick = [this]
    {
        juce::Component::SafePointer<UserPage> safe (this);
        UI::showInstrumentMenu (&newButton, proc.getInstrumentList(), {}, true, [safe] (const juce::String& inst)
        {
            if (safe == nullptr) return;
            const int u = inst.isEmpty() ? safe->proc.createUser ("User") : safe->proc.createUserFromInstrument (inst);
            safe->selectUser (u);
        });
    };
    copyButton.setButtonText ("Kopiraj");
    copyButton.onClick = [this]
    {
        if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
        auto u = proc.getUser (userIndex);
        selectUser (proc.createUser (u.name, &u));
    };
    deleteButton.setButtonText (T ("Obriši"));
    deleteButton.onClick = [this]
    {
        if (userIndex < 0 || userIndex >= proc.getNumUsers()) return;
        juce::Component::SafePointer<UserPage> safe (this);
        const int idx = userIndex;
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("Obriši usera"),
            T ("Obrisati usera \"") + proc.getUser (idx).name + T ("\"? Delovi performansa koji ga koriste ostaće bez zvuka."),
            T ("Obriši"), T ("Otkaži"), nullptr,
            juce::ModalCallbackFunction::create ([safe, idx] (int result)
            {
                if (safe == nullptr || result == 0) return;
                safe->proc.deleteUser (idx);
                safe->selectUser (juce::jmax (0, idx - 1));
            }));
    };
    for (auto* b : { &newButton, &copyButton, &deleteButton })
        addAndMakeVisible (b);

    nameEdit.setEditable (true);
    nameEdit.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    nameEdit.setColour (juce::Label::backgroundColourId, Pal::lcdBg);
    nameEdit.setColour (juce::Label::textColourId, Pal::lcdText);
    nameEdit.setTooltip (T ("Klikni da promeniš ime usera"));
    nameEdit.onTextChange = [this] { edit ([this] (UserProgram& u) { u.name = nameEdit.getText(); }); };
    addAndMakeVisible (nameEdit);

    const char* headers[] = { "Multisempl", "", "", "Jačina dB", "Pan", "Transp", "Fino c", "Od", "Do", "Vel od / do" };
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
    list.updateContent();
    list.selectRow (userIndex, false, true);
    refresh();
}

int UserPage::getNumRows() { return proc.getNumUsers(); }

void UserPage::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= proc.getNumUsers()) return;
    if (selected) g.fillAll (Pal::accent.withAlpha (0.35f));
    g.setColour (Pal::text);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (proc.getUser (row).name, 8, 0, w - 10, h, juce::Justification::centredLeft, true);
}

void UserPage::selectedRowsChanged (int lastRowSelected)
{
    if (lastRowSelected >= 0 && lastRowSelected != userIndex)
    {
        userIndex = lastRowSelected;
        refresh();
    }
}

void UserPage::refresh()
{
    list.updateContent();
    list.repaint();
    if (userIndex >= proc.getNumUsers())
        userIndex = juce::jmax (0, proc.getNumUsers() - 1);
    if (list.getSelectedRow() != userIndex && proc.getNumUsers() > 0)
        list.selectRow (userIndex, true, true);

    const bool valid = userIndex >= 0 && userIndex < proc.getNumUsers();
    for (auto* r : rows)
        r->refresh();
    sound.refresh();

    for (auto* c : std::initializer_list<juce::Component*> { &nameEdit, &copyButton, &deleteButton })
        c->setEnabled (valid);
    if (! valid)
    {
        nameEdit.setText (T ("Nema usera: klikni Novi"), juce::dontSendNotification);
        return;
    }
    if (! nameEdit.isBeingEdited())
        nameEdit.setText (proc.getUser (userIndex).name, juce::dontSendNotification);
}

void UserPage::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    r.removeFromLeft (222);
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r.toFloat(), 8.0f);
}

void UserPage::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (210);
    r.removeFromLeft (12);
    auto btns = left.removeFromBottom (30);
    const int bw = btns.getWidth() / 3;
    newButton.setBounds (btns.removeFromLeft (bw).reduced (2, 0));
    copyButton.setBounds (btns.removeFromLeft (bw).reduced (2, 0));
    deleteButton.setBounds (btns.reduced (2, 0));
    left.removeFromBottom (6);
    list.setBounds (left);

    r.reduce (10, 8);
    nameEdit.setBounds (r.removeFromTop (30).removeFromLeft (420));
    r.removeFromTop (4);

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
    notes[0].setTooltip (T ("Tonika akorda (uvek uključena)"));

    scale.onChange = [this]
    {
        const int s = scale.getSelectedItemIndex();
        owner.edit ([this, s] (TercaSettings& t) { if (index < (int) t.rules.size()) t.rules[(size_t) index].scale = s; });
    };
    addAndMakeVisible (scale);

    UI::setupIncDec (shift, -11, 11, 1, semis);
    shift.setTooltip (T ("Pomeraj tonaliteta od tonike akorda. Npr. E7 sa +5 daje A mol."));
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
    example.setText (T ("npr. ") + chord.joinIntoString (" ") + "  ->  " + Terca::keyName (2 + r.rootShift, t, r.scale),
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
    : proc (p), velocity (T ("Jačina terce"), juce::Colour (0xff3ddc84), 0.1, 1.5, 0.0, UI::pctText, 0.8)
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

    learnButton.setButtonText (T ("LEARN dugme"));
    learnButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    learnButton.onClick = [this] { proc.startLearn (! proc.isLearning()); };
    addAndMakeVisible (learnButton);
    learnLabel.setFont (juce::FontOptions (13.5f));
    addAndMakeVisible (learnLabel);

    voiceMode.addItemList ({ "Terca iznad", "Terca ispod", "Iznad i ispod" }, 1);
    voiceMode.onChange = [this] { const int v = voiceMode.getSelectedItemIndex(); edit ([v] (TercaSettings& t) { t.voiceMode = v; }); };
    addAndMakeVisible (voiceMode);

    velocity.onChange = [this] (double v) { edit ([v] (TercaSettings& t) { t.velocity = (float) v; }); };
    addAndMakeVisible (velocity);

    topOnly.setButtonText (T ("Samo na najviši ton (melodija)"));
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
    scaleName.setTooltip (T ("Klikni da promeniš ime skale"));
    scaleName.onTextChange = [this]
    {
        const auto n = scaleName.getText().trim();
        if (n.isEmpty()) return;
        edit ([this, n] (TercaSettings& t) { if (scaleIndex < (int) t.scales.size()) t.scales[(size_t) scaleIndex].name = n; });
    };
    addAndMakeVisible (scaleName);

    addScale.setButtonText ("Nova skala");
    addScale.onClick = [this]
    {
        edit ([this] (TercaSettings& t)
        {
            auto copy = t.scales.empty() ? TercaSettings::makeDefault().scales[0] : t.scales[(size_t) juce::jmin (scaleIndex, (int) t.scales.size() - 1)];
            copy.name = "User skala " + juce::String ((int) t.scales.size() + 1);
            t.scales.push_back (copy);
        });
        scaleIndex = (int) proc.getTerca().scales.size() - 1;
        refresh();
    };
    deleteScale.setButtonText (T ("Obriši"));
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

    styleLabel (aboveLabel, "IZNAD", 12.0f, Pal::text);
    styleLabel (belowLabel, "ISPOD", 12.0f, Pal::text);
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
    addRule.setButtonText ("Dodaj pravilo");
    addRule.onClick = [this]
    {
        edit ([] (TercaSettings& t) { t.rules.push_back ({ 1 | (1 << 4) | (1 << 7), 0, 0 }); });
        rebuildRules();
    };
    addAndMakeVisible (addRule);
    rulesHelp.setText (T ("Uključi tonove akorda (od tonike). Kad držiš TERCA dugme i odsviraš takav akord, uključuje se izabrana skala. "
                          "Pravila se proveravaju redom, prvo koje odgovara pobeđuje."), juce::dontSendNotification);
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
    onButton.setButtonText (on ? "TERCE: ON" : "TERCE: OFF");
    stateLabel.setText (proc.getTercaText(), juce::dontSendNotification);
    keyRoot.setSelectedItemIndex (proc.getTercaRoot(), juce::dontSendNotification);
    keyScale.clear (juce::dontSendNotification);
    for (auto& sc : t.scales)
        keyScale.addItem (sc.name, keyScale.getNumItems() + 1);
    keyScale.setSelectedItemIndex (proc.getTercaScale(), juce::dontSendNotification);

    learnButton.setToggleState (proc.isLearning(), juce::dontSendNotification);
    learnLabel.setText (proc.isLearning() ? T ("Pritisni dugme na klavijaturi...") : T ("Dugme: ") + t.learn.describe(),
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

    UI::sectionTitle (g, left.reduced (12, 0).withHeight (26).withY (left.getY() + 4), "TERCE", juce::Colour (0xff3ddc84));
    UI::sectionTitle (g, scalesArea.reduced (12, 0).withHeight (26).withY (scalesArea.getY() + 4),
                      T ("SKALE: za svaki ton koliko polutonova gore / dole (0 = bez terce)"), Pal::lcdText);
    UI::sectionTitle (g, r.reduced (12, 0).withHeight (26).withY (r.getY() + 4),
                      T ("PRAVILA: koji akord (ili dva tona) pali koju skalu"), Pal::lcdText);
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
    add ("PLATE REVERB", { { "Veličina", 0, 1, 0, 0.5, UI::pctText }, { "Trajanje", 0.3, 10, 2, 2.2, sec },
                           { "Predelay", 0, 150, 30, 20, ms }, { "Prigušenje", 0, 1, 0, 0.5, UI::pctText },
                           { "Low cut", 20, 1000, 150, 150, UI::hzText }, { "High cut", 1000, 16000, 5000, 6000, UI::hzText },
                           { "Gustina", 0, 1, 0, 0.7, UI::pctText }, { "Modulacija", 0, 1, 0, 0.3, UI::pctText },
                           { "Širina", 0, 1, 0, 1.0, UI::pctText }, { "Nivo", 0, 1, 0, 0.8, UI::pctText } });
    add ("DELAY", { { "Vreme", 20, 1500, 350, 350, [] (double v) { return juce::String (juce::roundToInt (v)) + " ms"; } },
                    { "Ponavljanje", 0, 0.95, 0, 0.35, UI::pctText }, { "Nivo", 0, 1, 0, 0.6, UI::pctText } });
    add ("EQ", { { "Bas", -12, 12, 0, 0, db }, { "Sredina", -12, 12, 0, 0, db }, { "Visoki", -12, 12, 0, 0, db } });
    add ("KOMPRESOR", { { "Prag", -40, 0, 0, -12, db }, { "Odnos (1=isklj.)", 1, 10, 3, 1, [] (double v) { return juce::String (v, 1) + ":1"; } } });
    add ("PERFORMANS", { { "Jačina", -60, 6, -12, 0, UI::dbText } });

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
    setter (k++, [] (MasterSettings& m, float v) { m.compThresholdDb = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.compRatio = v; });
    setter (k++, [] (MasterSettings& m, float v) { m.masterDb = v; });

    globalVolume = std::make_unique<ValueKnob> ("MASTER", juce::Colours::white, -60.0, 6.0, -12.0, UI::dbText, 0.0);
    globalVolume->onChange = [this] (double v) { proc.setGlobalVolume ((float) v); };
    addAndMakeVisible (*globalVolume);
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
                           m.eqLowDb, m.eqMidDb, m.eqHighDb, m.compThresholdDb, m.compRatio, m.masterDb };
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
    UI::sectionTitle (g, juce::Rectangle<int> (gv.getX(), gv.getY() - 34, gv.getWidth(), 24), T ("MASTER (svi performansi)"), juce::Colours::white);
    g.setColour (Pal::dim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (T ("Efekti se čuvaju u performansu. Koliko svaki deo šalje u reverb i delay podešava se na stranici PERFORMANS."),
                getLocalBounds().reduced (20).removeFromBottom (24), juce::Justification::centredLeft);
}

void EffectsPage::resized()
{
    auto r = getLocalBounds().reduced (24, 16);
    r.removeFromBottom (28);
    const int rowH = r.getHeight() / 3;
    const int kw = juce::jmin (110, r.getWidth() / 10), kh = juce::jmin (110, rowH - 40), gap = 30;
    // red 1: reverb, red 2: delay + EQ, red 3: kompresor + performans + master
    const int rowOf[] = { 0, 1, 1, 2, 2 };
    int x = 0, lastRow = -1, y = 0;
    for (int si = 0; si < sections.size(); ++si)
    {
        const int row = rowOf[juce::jmin (si, 4)];
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

    addButton.setButtonText ("Dodaj folder / USB");
    addButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (T ("Izaberi folder sa semplovima (može i ceo USB stick)"),
                                                       juce::File::getSpecialLocation (juce::File::userHomeDirectory), "*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.isDirectory())
                    proc.addLibraryFolder (f);
            });
    };
    removeButton.setButtonText ("Ukloni");
    removeButton.onClick = [this] { proc.removeLibraryFolder (folders.getSelectedRow()); };
    rescanButton.setButtonText (T ("Osveži (ubačen USB)"));
    rescanButton.onClick = [this] { proc.rescanLibrary(); };
    for (auto* b : { &addButton, &removeButton, &rescanButton })
        addAndMakeVisible (b);

    c3Toggle.setButtonText (T ("C3 = MIDI 60 (Yamaha/Korg). Isključeno: C4 = 60"));
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
    g.drawText (f.getFullPathName() + (f.isDirectory() ? juce::String() : T ("   (nije dostupan)")), 10, 0, w - 12, h,
                juce::Justification::centredLeft, true);
}

void SettingsPage::refresh()
{
    folders.updateContent();
    folders.repaint();
    c3Toggle.setToggleState (proc.getC3Is60(), juce::dontSendNotification);
    info.setText (T ("Svaki folder se pretražuje sa podfolderima: WAV folderi, Korg KMP i Kontakt NKI postaju instrumenti.\n"
                     "USB stick: ubaci ga, klikni \"Dodaj folder / USB\" i izaberi stick. Ako ga izvadiš i vratiš, klikni \"Osveži\".\n"
                     "Zvučna kartica i MIDI klavijatura: dugme Options u gornjem levom uglu prozora.\n\n"
                     "Useri, performansi i terce se čuvaju automatski u:\n") + proc.getDataFile().getFullPathName(),
                  juce::dontSendNotification);
}

void SettingsPage::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    UI::sectionTitle (g, getLocalBounds().reduced (20, 0).withHeight (26).withY (8), "FOLDERI SA SEMPLOVIMA", Pal::lcdText);
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
    c3Toggle.setBounds (r.removeFromTop (28));
    r.removeFromTop (16);
    info.setBounds (r);
}

//==============================================================================
//  KronosPage
//==============================================================================
enum KronosCols { kcBank = 1, kcNum, kcName, kcOsc1, kcOsc2, kcState };

KronosPage::KronosPage (LiveSamplerProcessor& p, LiveSamplerEditor& e) : proc (p), editor (e)
{
    loadButton.setButtonText (T ("Učitaj Kronos PCG..."));
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (T ("Izaberi Kronos PCG fajl"),
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

    onlyUser.setButtonText (T ("Samo programi sa mojim semplovima"));
    onlyUser.setToggleState (true, juce::dontSendNotification);
    onlyUser.onClick = [this] { rebuildRows(); };
    addAndMakeVisible (onlyUser);

    keepChannels.setButtonText (T ("Zadrži MIDI kanale iz kombija"));
    keepChannels.setTooltip (T ("Isključeno: svi delovi sviraju sa tvoje klavijature, na bilo kom kanalu. Uključi samo ako sviraš sa više klavijatura na različitim kanalima."));
    addChildComponent (keepChannels);

    search.setTextToShowWhenEmpty (T ("Traži po imenu..."), Pal::dim);
    search.onTextChange = [this] { rebuildRows(); };
    addAndMakeVisible (search);

    table.setModel (this);
    table.setRowHeight (24);
    table.setMultipleSelectionEnabled (true);
    table.setColour (juce::ListBox::backgroundColourId, Pal::bg);
    auto& h = table.getHeader();
    h.addColumn ("Banka", kcBank, 80, 60, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Br.", kcNum, 50, 40, 80, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Program", kcName, 230, 120, 400, juce::TableHeaderComponent::notSortable);
    h.addColumn ("OSC1 multisempl", kcOsc1, 260, 120, 500, juce::TableHeaderComponent::notSortable);
    h.addColumn ("OSC2 multisempl", kcOsc2, 260, 120, 500, juce::TableHeaderComponent::notSortable);
    h.addColumn ("User", kcState, 150, 80, 300, juce::TableHeaderComponent::notSortable);
    h.setColour (juce::TableHeaderComponent::backgroundColourId, Pal::panelHi);
    h.setColour (juce::TableHeaderComponent::textColourId, Pal::text);
    addAndMakeVisible (table);

    createSelected.setButtonText (T ("Napravi usere od izabranih"));
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
    createAll.setButtonText (T ("Napravi usere od svih prikazanih"));
    createAll.onClick = [this]
    {
        juce::Component::SafePointer<KronosPage> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Kronos",
            T ("Napraviti usere od ") + juce::String ((int) rows.size()) + T (" programa? Postojeći useri sa istim imenom se ne dupliraju."),
            T ("Napravi"), T ("Otkaži"), nullptr,
            juce::ModalCallbackFunction::create ([safe] (int r)
            {
                if (safe == nullptr || r == 0 || safe->proc.getKronosPcg() == nullptr) return;
                for (int idx : safe->rows)
                    safe->userFor (safe->proc.getKronosPcg()->programs[(size_t) idx]);
                safe->table.repaint();
            }));
    };
    playButton.setButtonText (T ("Sviraj (PART 1)"));
    playButton.setTooltip (T ("Napravi usera i stavi ga na PART 1 trenutnog performansa. Može i dupli klik na program."));
    playButton.onClick = [this] { const int r = table.getSelectedRow(); if (r < 0) return; if (combiMode) loadCombiRow (r); else playRow (r); };
    const char* modeNames[] = { "PROGRAMI", "KOMBIJI" };
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

    help.setText (T ("Programi: dupli klik pravi usera (OSC1/OSC2, zone, filter, EG...) i stavlja ga na PART 1. "
                     "Kombiji: dupli klik pravi performans od 16 timbre-a; fabrički ROM programi se preskaču."),
                  juce::dontSendNotification);
    help.setFont (juce::FontOptions (12.5f));
    help.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (help);
}

void KronosPage::loadPcg (const juce::File& f)
{
    juce::String err;
    if (! proc.loadKronosPcg (f, err))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Kronos", T ("Ne mogu da učitam PCG: ") + err);
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
    h.setColumnName (kcName, combis ? T ("Kombi") : T ("Program"));
    h.setColumnName (kcOsc1, combis ? T ("Timbre-i (Play)") : T ("OSC1 multisempl"));
    h.setColumnName (kcOsc2, combis ? T ("Fabrički (ROM)") : T ("OSC2 multisempl"));
    h.setColumnName (kcState, combis ? T ("Kanal") : T ("User"));
    h.setColumnWidth (kcOsc1, combis ? 520 : 260);
    h.setColumnWidth (kcOsc2, combis ? 120 : 260);
    onlyUser.setButtonText (combis ? T ("Samo kombiji sa mojim semplovima") : T ("Samo programi sa mojim semplovima"));
    playButton.setButtonText (combis ? T ("Učitaj u performans") : T ("Sviraj (PART 1)"));
    playButton.setTooltip (combis ? T ("Kombi postaje trenutni performans (16 delova). Može i dupli klik. Sačuvaj ga dugmetom SNIMI.")
                                  : T ("Napravi usera i stavi ga na PART 1 trenutnog performansa. Može i dupli klik na program."));
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

void KronosPage::offerAutosample (const juce::String& message, const juce::String& args)
{
    juce::Component::SafePointer<KronosPage> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Kronos", message,
        T ("Presempluj sa Kronosa"), T ("Zatvori"), nullptr,
        juce::ModalCallbackFunction::create ([safe, args] (int r)
        {
            if (safe == nullptr || r == 0) return;
            const auto exe = findAutosampler();
            if (! exe.existsAsFile())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Kronos",
                    T ("Ne nalazim \"Pikaso Autosampler.exe\". Raspakuj PikasoAutosampler-Windows pored Pikaso Workstation ili na Desktop."));
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
    const auto skipped = proc.loadKronosCombi (combi, keepChannels.getToggleState());
    editor.showTab (LiveSamplerEditor::TabPerformance);
    if (skipped.isEmpty())
        return;
    bool rom = false;
    for (auto& s : skipped) rom = rom || s.contains (T ("fabrički ROM")) || s.contains ("EXi ");
    const auto msg = T ("Ovi timbre-i nisu preneti:\n\n") + skipped.joinIntoString ("\n");
    if (! rom)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Kronos", msg);
        return;
    }
    offerAutosample (msg + T ("\n\nDa ih presemplujem sa Kronosa (Pikaso Autosampler)? ROM programima se snimaju multisemplovi, a EXi programima (AL-1, CX-3, SGX-2...) ceo zvuk."),
                     "--pcg \"" + pcg->file.getFullPathName() + "\" --combi " + juce::String (combi.bankId) + ":" + juce::String (combi.number));
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
            offerAutosample (T ("Program \"") + prog.name + T ("\" je EXi program (") + prog.engineText()
                             + T (" sinteza) i još nije presemplovan.\n\nDa ga presemplujem sa Kronosa (Pikaso Autosampler)? Snima se ceo zvuk programa, svaki treći ton, u više jačina."),
                             "--pcg \"" + pcg->file.getFullPathName() + "\" --program " + juce::String (prog.bankId) + ":" + juce::String (prog.number));
        else if (prog.drumKit)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Kronos", T ("Ovo je drum kit program; drum kitovi još nisu podržani."));
        else if (rom)
            offerAutosample (T ("Program \"") + prog.name + T ("\" koristi fabričke ROM multisemplove koji još nisu presemplovani.\n\nDa ih presemplujem sa Kronosa (Pikaso Autosampler)?"),
                             "--pcg \"" + pcg->file.getFullPathName() + "\" --program " + juce::String (prog.bankId) + ":" + juce::String (prog.number));
        else
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Kronos",
                T ("Semplovi ovog programa nisu u biblioteci. Proveri da je KSC kolekcija u folderu biblioteke (PODEŠAVANJA)."));
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
        fileLabel.setText (T ("Nije učitan PCG"), juce::dontSendNotification);
        infoLabel.setText (T ("Učitaj PCG sa Kronosa. KSC fajl i folder sa semplovima treba da budu pored njega."), juce::dontSendNotification);
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
    infoLabel.setText (juce::String ((int) pcg->programs.size()) + T (" programa (") + juce::String (hd1) + T (" HD-1, ") + juce::String (exi) + T (" EXi), ")
                       + juce::String (user) + T (" sa tvojim semplovima  |  ") + juce::String ((int) pcg->combis.size()) + T (" kombija  |  ")
                       + juce::String (pcg->drumKits) + T (" bubanj kitova") + (proc.isScanning() ? T ("  |  pretražujem semplove...") : juce::String()),
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
    if (inst.isEmpty()) return "MS" + juce::String (z.ms) + T ("  (nije u biblioteci)");
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
                t = ch.joinIntoString (", ");
                c = Pal::dim;
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
                else t = inst.isNotEmpty() ? T ("presemplovan") : T ("nije presemplovan");
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
            const int u = proc.findUser (p.name);
            t = u >= 0 ? T ("✓ napravljen") : "";
            c = juce::Colour (0xff3ddc84);
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
    writeButton.setButtonText ("SNIMI");
    writeButton.setClickingTogglesState (true);
    writeButton.setColour (juce::TextButton::buttonOnColourId, Pal::red);
    writeButton.setTooltip (T ("Uključi SNIMI pa klikni mesto da sačuvaš trenutni performans"));
    writeButton.onClick = [this] { repaint(); };
    addAndMakeVisible (writeButton);

    styleLabel (nameLabel, T ("IME LISTE"), 12.5f);
    addAndMakeVisible (nameLabel);
    nameEdit.setEditable (true);
    nameEdit.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    nameEdit.setColour (juce::Label::backgroundColourId, Pal::bg);
    nameEdit.setColour (juce::Label::textColourId, Pal::lcdText);
    nameEdit.onTextChange = [this] { proc.setBankName (proc.getCurrentBank(), nameEdit.getText()); };
    addAndMakeVisible (nameEdit);

    styleLabel (filterLabel, T ("KATEGORIJA"), 12.5f);
    addAndMakeVisible (filterLabel);
    catFilter.onChange = [this] { repaint(); };
    addAndMakeVisible (catFilter);
    categoriesButton.setButtonText (T ("KATEGORIJE..."));
    categoriesButton.setTooltip (T ("Izmeni, dodaj, obriši ili preuredi kategorije"));
    categoriesButton.onClick = [this] { categoriesMenu(); };
    addAndMakeVisible (categoriesButton);
    missingLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    missingLabel.setColour (juce::Label::textColourId, juce::Colour (0xffff8a3d));
    addAndMakeVisible (missingLabel);

    help.setText (T ("Klik: učitaj. SNIMI + klik: sačuvaj trenutni performans. Desni klik: meni (kategorija, snimi, u set listu, obriši). "
                     "Narandžast okvir = performans bez kategorije. "
                     "MIDI: Bank Select (CC0) bira listu, Program Change 0-127 performans."), juce::dontSendNotification);
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
    catFilter.addItem (T ("Sve"), 1);
    catFilter.addItem (T ("Bez kategorije"), 2);
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
        bankButtons[b].setTooltip (proc.getBankName (b) + (n > 0 ? "  (" + juce::String (n) + T (" bez kategorije)") : juce::String()));
    }
    missingLabel.setText (all == 0 ? juce::String() : juce::String (here) + T (" bez kategorije u ovoj listi, ") + juce::String (all) + T (" u svim listama"),
                          juce::dontSendNotification);
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
            const auto cat = missing ? T ("BEZ KAT.") : s.category;
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
        g.drawText (juce::String (i + 1).paddedLeft ('0', 3) + "  " + (s.used ? s.name : juce::String()),
                    textArea, juce::Justification::centredLeft, true);
    }
}

void ListsPage::mouseDown (const juce::MouseEvent& e)
{
    const int i = cellAt (e.getPosition());
    if (i < 0) return;
    const int slot = proc.getCurrentBank() * kPerfPerBank + i;
    if (e.mods.isPopupMenu()) { cellMenu (i); return; }
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

void ListsPage::cellMenu (int i)
{
    const int slot = proc.getCurrentBank() * kPerfPerBank + i;
    const bool used = proc.getSlot (slot).used;
    juce::PopupMenu m;
    m.addItem (1, T ("Učitaj"), used);
    {
        juce::PopupMenu cm;
        const auto& cats = proc.getCategories();
        const int cur = proc.categoryIndex (proc.getSlot (slot));
        for (int c = 0; c < cats.size(); ++c)
            cm.addItem (1000 + c, cats[c], true, cur == c);
        cm.addSeparator();
        cm.addItem (999, T ("Bez kategorije"), true, cur < 0);
        cm.addItem (998, T ("Izmeni kategorije..."));
        m.addSubMenu (T ("Kategorija"), cm, used);
    }
    m.addItem (2, T ("Snimi trenutni performans ovde"));
    m.addItem (5, T ("Nov prazan (inicijalizovan) performans ovde"));
    m.addItem (3, T ("Dodaj u set listu (prvo prazno mesto)"), used);
    m.addSeparator();
    m.addItem (4, T ("Obriši"), used);
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
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("Nov performans"),
                    T ("Na ovom mestu je \"") + pr.getSlot (slot).name + T ("\". Zameniti ga praznim performansom?"),
                    T ("Zameni"), T ("Otkaži"), nullptr,
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
            btn (addB, T ("Dodaj"), [this]
            {
                names.add (T ("Nova kategorija"));
                colours.push_back (juce::Colour (0xff8a93a6));
                origin.push_back (-1);
                list.updateContent();
                list.selectRow (names.size() - 1);
                renameRow (names.size() - 1);
            });
            btn (renB, T ("Preimenuj"), [this] { renameRow (list.getSelectedRow()); });
            btn (delB, T ("Obriši"), [this]
            {
                const int r = list.getSelectedRow();
                if (r < 0 || r >= names.size()) return;
                names.remove (r);
                colours.erase (colours.begin() + r);
                origin.erase (origin.begin() + r);
                list.updateContent();
                list.repaint();
            });
            btn (upB, T ("Gore"), [this] { move (-1); });
            btn (downB, T ("Dole"), [this] { move (1); });
            btn (kronosB, T ("Boja..."), [this] { pickColour (list.getSelectedRow()); });
            btn (factoryB, T ("Kao Live Kontroler"), [this]
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
                for (auto& n : names) clean.add (n.trim().isEmpty() ? T ("Bez imena") : n.trim());
                proc.setCategories (clean, colours, remap);
                close();
            });
            btn (cancelB, T ("Otkaži"), [this] { close(); });
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
            auto* aw = new juce::AlertWindow (T ("Kategorija"), T ("Ime kategorije:"), juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor ("name", names[row]);
            aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton (T ("Otkaži"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
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
        m.addSectionHeader (T ("Povezano sa Live Kontrolerom (uređaj Pikaso Workstation)"));
        m.addItem (99, proc.getKontrolerFile().getFullPathName(), false);
        m.addItem (98, proc.getKontrolerStatus(), false);
        m.addItem (4, T ("Uredi kategorije..."));
        m.addItem (1, T ("Sinhronizuj sada"));
        m.addItem (2, T ("Izaberi drugi fajl Live Kontrolera..."));
        m.addSeparator();
        m.addItem (3, T ("Prekini vezu"));
    }
    else
    {
        m.addItem (4, T ("Uredi kategorije..."));
        m.addSeparator();
        m.addItem (2, T ("Poveži sa Live Kontrolerom (iste kategorije i boje)..."));
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
            safe->chooser = std::make_unique<juce::FileChooser> (T ("Live Kontroler podešavanja (podesavanja-server.json)"), start, "*.json");
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
    o.dialogTitle = T ("Kategorije performansa");
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
    nameEdit.setTooltip (T ("Ime set liste (dupli klik za izmenu)"));
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
    editButton.setTooltip (T ("Uključi EDIT pa klikni dugme: stavi performans, natpis, boju"));
    editButton.onClick = [this] { repaint(); };
    addAndMakeVisible (editButton);

    pcMode.setButtonText (T ("Program Change bira set listu"));
    pcMode.setTooltip (T ("Uključeno: CC0 bira set listu, Program Change 0-127 dugme. Isključeno: Program Change bira performans iz liste."));
    pcMode.onClick = [this] { proc.setProgramChangeMode (pcMode.getToggleState() ? 1 : 0); };
    addAndMakeVisible (pcMode);
}

void SetListPage::refresh()
{
    listSelect.clear (juce::dontSendNotification);
    for (int i = 0; i < kNumSetLists; ++i)
    {
        const auto& n = proc.getSetList (i).name;
        listSelect.addItem (juce::String (i + 1) + "  " + (n.isNotEmpty() ? n : T ("Set lista")), i + 1);
    }
    listSelect.setSelectedItemIndex (proc.getCurrentSetList(), juce::dontSendNotification);
    if (! nameEdit.isBeingEdited())
    {
        const auto& n = proc.getSetList (proc.getCurrentSetList()).name;
        nameEdit.setText (n.isNotEmpty() ? n : T ("Set lista ") + juce::String (proc.getCurrentSetList() + 1), juce::dontSendNotification);
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
        g.drawText (proc.getBankName (s.perf / kPerfPerBank) + " " + juce::String (s.perf % kPerfPerBank + 1).paddedLeft ('0', 3)
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
    m.addItem (1, T ("Stavi trenutni performans"), proc.getCurrentSlot() >= 0);
    for (int b = 0; b < kNumPerfBanks; ++b)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < kPerfPerBank; ++i)
            if (const auto& s = proc.getSlot (b * kPerfPerBank + i); s.used)
                sub.addItem (1000 + b * kPerfPerBank + i, juce::String (i + 1).paddedLeft ('0', 3) + "  " + s.name);
        if (sub.getNumItems() > 0)
            pick.addSubMenu (proc.getBankName (b), sub);
    }
    m.addSubMenu (T ("Izaberi performans"), pick);
    m.addItem (2, T ("Natpis na dugmetu..."));
    const char* cn[] = { "Siva", "Narandžasta", "Plava", "Zelena", "Ljubičasta", "Žuta", "Tirkizna", "Indigo" };
    for (int c = 0; c < 8; ++c)
        colours.addColouredItem (100 + c, T (cn[c]), slotColour (c).brighter (0.4f));
    m.addSubMenu (T ("Boja"), colours);
    m.addSeparator();
    m.addItem (3, T ("Isprazni dugme"));

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
            auto* w = new juce::AlertWindow (T ("Natpis"), T ("Tekst na dugmetu (prazno = ime performansa)"), juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("t", s.label);
            w->addButton ("OK", 1);
            w->addButton (T ("Otkaži"), 0);
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

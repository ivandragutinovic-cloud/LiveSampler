#include "Pages.h"
#include "Text.h"

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
    onButton.setBounds (r.removeFromTop (26));
    r.removeFromTop (4);
    userButton.setBounds (r.removeFromTop (40));
    r.removeFromTop (2);
    editButton.setBounds (r.removeFromTop (20));
    r.removeFromTop (4);

    auto knobs1 = r.removeFromTop (60);
    volume.setBounds (knobs1.removeFromLeft (knobs1.getWidth() / 2));
    pan.setBounds (knobs1);
    auto knobs2 = r.removeFromTop (60);
    reverb.setBounds (knobs2.removeFromLeft (knobs2.getWidth() / 2));
    delay.setBounds (knobs2);
    r.removeFromTop (4);

    juce::Slider* boxes[] = { &transpose, &octave, &keyLow, &keyHigh, &velLow, &velHigh, &channel };
    for (int k = 0; k < 7; ++k)
    {
        auto row = r.removeFromTop (25);
        labels[k].setBounds (row.removeFromLeft (46));
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
        addAndMakeVisible (strips.add (new PartStrip (p, e, i)));
    addAndMakeVisible (ranges);
}

void PerformancePage::refresh()
{
    if (! nameEdit.isBeingEdited())
        nameEdit.setText (proc.getPerformance().name, juce::dontSendNotification);
    for (auto* s : strips)
        s->refresh();
    ranges.repaint();
}

void PerformancePage::paint (juce::Graphics&) {}

void PerformancePage::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (32);
    nameLabel.setBounds (top.removeFromLeft (140));
    nameEdit.setBounds (top.removeFromLeft (360).reduced (0, 2));

    r.removeFromTop (4);
    ranges.setBounds (r.removeFromBottom (78));
    r.removeFromBottom (6);

    const int gap = 6;
    const int w = (r.getWidth() - gap * (kNumParts - 1)) / kNumParts;
    for (auto* s : strips)
    {
        s->setBounds (r.removeFromLeft (w));
        r.removeFromLeft (gap);
    }
}

//==============================================================================
//  OscRow
//==============================================================================
OscRow::OscRow (LiveSamplerProcessor& p, LiveSamplerEditor& e, int o, std::function<int()> ui)
    : proc (p), editor (e), osc (o), userIndex (std::move (ui))
{
    onButton.setButtonText ("OSC " + juce::String (o + 1));
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
        UI::showInstrumentMenu (&instButton, proc.getInstrumentList(), u.osc[(size_t) osc].instrument, true,
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
    mapButton.onClick = [this] { editor.editMapping (proc.getUser (userIndex()).name, osc); };
    addAndMakeVisible (mapButton);

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

void OscRow::edit (std::function<void (OscSettings&)> fn)
{
    const int u = userIndex();
    if (u < 0 || u >= proc.getNumUsers()) return;
    auto user = proc.getUser (u);
    fn (user.osc[(size_t) osc]);
    proc.updateUser (u, user);
}

void OscRow::refresh()
{
    const int u = userIndex();
    const bool valid = u >= 0 && u < proc.getNumUsers();
    setEnabled (valid);
    if (! valid) return;

    const auto& s = proc.getUser (u).osc[(size_t) osc];
    onButton.setToggleState (s.on, juce::dontSendNotification);
    auto instText = s.instrument.isEmpty() ? T ("— instrument —") : s.instrument.fromFirstOccurrenceOf ("/", false, false);
    if (s.instrument.isNotEmpty() && ! proc.getInstrumentList().contains (s.instrument))
        instText = T ("⚠ nema: ") + s.instrument;
    instButton.setButtonText (instText);
    mapButton.setEnabled (s.instrument.isNotEmpty());
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
    g.setColour (osc % 2 == 0 ? Pal::panel : Pal::panel.brighter (0.03f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
}

void OscRow::resized()
{
    auto r = getLocalBounds().reduced (4, 3);
    onButton.setBounds (r.removeFromLeft (64));
    r.removeFromLeft (6);
    const int boxW = 86;
    juce::Slider* boxes[] = { &level, &pan, &transpose, &fine, &keyLow, &keyHigh, &velLow, &velHigh };
    const int fixed = boxW * 8 + 56 + 6;
    auto instArea = r.removeFromLeft (r.getWidth() - fixed);
    instButton.setBounds (instArea);
    r.removeFromLeft (4);
    mapButton.setBounds (r.removeFromLeft (52));
    r.removeFromLeft (2);
    for (auto* b : boxes)
    {
        b->setBounds (r.removeFromLeft (boxW).reduced (2, 0));
        b->setTextBoxStyle (juce::Slider::TextBoxLeft, false, boxW - 38, r.getHeight());
    }
}

//==============================================================================
//  UserPage
//==============================================================================
UserPage::UserPage (LiveSamplerProcessor& p, LiveSamplerEditor& e)
    : proc (p), editor (e)
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

    const char* headers[] = { "Instrument", "", "Jačina dB", "Pan", "Transp", "Fino c", "Od", "Do", "Vel od / do" };
    for (int i = 0; i < 9; ++i)
    {
        styleLabel (headerLabels[i], T (headers[i]), 11.5f);
        addAndMakeVisible (headerLabels[i]);
    }

    for (int o = 0; o < kNumOsc; ++o)
        addAndMakeVisible (rows.add (new OscRow (p, e, o, [this] { return userIndex; })));

    struct K { const char* name; double lo, hi, centre, def; std::function<juce::String (double)> fmt; };
    const K ks[] = {
        { "Attack", 0.001, 5.0, 0.3, 0.001, UI::timeText }, { "Decay", 0.01, 5.0, 0.5, 0.5, UI::timeText },
        { "Sustain", 0.0, 1.0, 0.0, 1.0, UI::pctText }, { "Release", 0.01, 8.0, 0.6, 0.3, UI::timeText },
        { "Cutoff", 40.0, 20000.0, 1500.0, 20000.0, UI::hzText }, { "Rezonanca", 0.0, 1.0, 0.0, 0.0, UI::pctText },
        { "Velocity", 0.0, 1.0, 0.0, 0.8, UI::pctText }, { "Portamento", 0.0, 2.0, 0.25, 0.0, UI::timeText },
        { "FX brzina", 0.0, 1.0, 0.0, 0.3, UI::pctText }, { "FX dubina", 0.0, 1.0, 0.0, 0.5, UI::pctText },
        { "FX mix", 0.0, 1.0, 0.0, 0.5, UI::pctText } };
    for (auto& k : ks)
        addAndMakeVisible (knobs.add (new ValueKnob (T (k.name), Pal::accent, k.lo, k.hi, k.centre, k.fmt, k.def)));

    knobs[0]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.attack = (float) v; }); };
    knobs[1]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.decay = (float) v; }); };
    knobs[2]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.sustain = (float) v; }); };
    knobs[3]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.release = (float) v; }); };
    knobs[4]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.cutoff = (float) v; }); };
    knobs[5]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.resonance = (float) v; }); };
    knobs[6]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.velSens = (float) v; }); };
    knobs[7]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.portamento = (float) v; }); };
    knobs[8]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.fxRate = (float) v; }); };
    knobs[9]->onChange  = [this] (double v) { edit ([v] (UserProgram& u) { u.fxDepth = (float) v; }); };
    knobs[10]->onChange = [this] (double v) { edit ([v] (UserProgram& u) { u.fxMix = (float) v; }); };

    monoButton.setButtonText ("MONO");
    monoButton.setClickingTogglesState (true);
    monoButton.setColour (juce::TextButton::buttonOnColourId, Pal::accent);
    monoButton.onClick = [this] { edit ([this] (UserProgram& u) { u.mono = monoButton.getToggleState(); }); };
    addAndMakeVisible (monoButton);

    UI::setupIncDec (bendRange, 0, 12, 1);
    bendRange.onValueChange = [this] { edit ([this] (UserProgram& u) { u.bendRange = (int) bendRange.getValue(); }); };
    styleLabel (bendLabel, "P.bend", 12.0f);
    addAndMakeVisible (bendRange);
    addAndMakeVisible (bendLabel);

    fxType.addItemList (InsertFx::getTypeNames(), 1);
    fxType.onChange = [this] { edit ([this] (UserProgram& u) { u.fxType = fxType.getSelectedItemIndex(); }); };
    styleLabel (fxLabel, "INSERT EFEKAT", 12.0f);
    addAndMakeVisible (fxType);
    addAndMakeVisible (fxLabel);
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

    for (auto* c : std::initializer_list<juce::Component*> { &nameEdit, &copyButton, &deleteButton, &monoButton, &bendRange, &fxType })
        c->setEnabled (valid);
    for (auto* k : knobs)
        k->setEnabled (valid);
    if (! valid)
    {
        nameEdit.setText (T ("Nema usera: klikni Novi"), juce::dontSendNotification);
        return;
    }

    const auto& u = proc.getUser (userIndex);
    if (! nameEdit.isBeingEdited())
        nameEdit.setText (u.name, juce::dontSendNotification);
    const float vals[] = { u.attack, u.decay, u.sustain, u.release, u.cutoff, u.resonance, u.velSens, u.portamento, u.fxRate, u.fxDepth, u.fxMix };
    for (int i = 0; i < knobs.size(); ++i)
        knobs[i]->setValue (vals[i]);
    monoButton.setToggleState (u.mono, juce::dontSendNotification);
    setSliderValue (bendRange, u.bendRange);
    fxType.setSelectedItemIndex (u.fxType, juce::dontSendNotification);
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
    nameEdit.setBounds (r.removeFromTop (34).removeFromLeft (420));
    r.removeFromTop (6);

    // zaglavlje kolona (prati raspored u OscRow)
    auto header = r.removeFromTop (16).reduced (4, 0);
    header.removeFromLeft (70);
    const int boxW = 86;
    const int fixed = boxW * 8 + 56 + 6;
    headerLabels[0].setBounds (header.removeFromLeft (header.getWidth() - fixed));
    header.removeFromLeft (4);
    headerLabels[1].setBounds (header.removeFromLeft (52));
    header.removeFromLeft (2);
    for (int i = 2; i < 8; ++i)
        headerLabels[i].setBounds (header.removeFromLeft (boxW));
    headerLabels[8].setBounds (header);

    for (auto* row : rows)
    {
        row->setBounds (r.removeFromTop (32));
        r.removeFromTop (2);
    }

    r.removeFromTop (10);
    auto kRow = r.removeFromTop (80);
    const int kw = 92;
    for (int i = 0; i < 8; ++i)
        knobs[i]->setBounds (kRow.removeFromLeft (kw));
    kRow.removeFromLeft (16);
    auto monoArea = kRow.removeFromLeft (110);
    monoButton.setBounds (monoArea.removeFromTop (30).reduced (2, 0));
    monoArea.removeFromTop (4);
    bendLabel.setBounds (monoArea.removeFromTop (16));
    bendRange.setBounds (monoArea.removeFromTop (24));
    bendRange.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 46, 24);

    r.removeFromTop (8);
    auto fxRow = r.removeFromTop (80);
    auto fxArea = fxRow.removeFromLeft (200);
    fxArea.removeFromTop (14);
    fxLabel.setBounds (fxArea.removeFromTop (16));
    fxType.setBounds (fxArea.removeFromTop (28).withTrimmedRight (16));
    for (int i = 8; i < 11; ++i)
        knobs[i]->setBounds (fxRow.removeFromLeft (kw));
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

    add ("REVERB", { { "Veličina", 0, 1, 0, 0.5, UI::pctText }, { "Prigušenje", 0, 1, 0, 0.5, UI::pctText }, { "Nivo", 0, 1, 0, 0.8, UI::pctText } });
    add ("DELAY", { { "Vreme", 20, 1500, 350, 350, [] (double v) { return juce::String (juce::roundToInt (v)) + " ms"; } },
                    { "Ponavljanje", 0, 0.95, 0, 0.35, UI::pctText }, { "Nivo", 0, 1, 0, 0.6, UI::pctText } });
    add ("EQ", { { "Bas", -12, 12, 0, 0, db }, { "Sredina", -12, 12, 0, 0, db }, { "Visoki", -12, 12, 0, 0, db } });
    add ("KOMPRESOR", { { "Prag", -40, 0, 0, -12, db }, { "Odnos (1=isklj.)", 1, 10, 3, 1, [] (double v) { return juce::String (v, 1) + ":1"; } } });
    add ("PERFORMANS", { { "Jačina", -60, 6, -12, 0, UI::dbText } });

    auto setter = [this] (int i, std::function<void (MasterSettings&, float)> fn)
    {
        knobs[i]->onChange = [this, fn] (double v) { edit ([fn, v] (MasterSettings& m) { fn (m, (float) v); }); };
    };
    setter (0,  [] (MasterSettings& m, float v) { m.reverbSize = v; });
    setter (1,  [] (MasterSettings& m, float v) { m.reverbDamping = v; });
    setter (2,  [] (MasterSettings& m, float v) { m.reverbLevel = v; });
    setter (3,  [] (MasterSettings& m, float v) { m.delayTimeMs = v; });
    setter (4,  [] (MasterSettings& m, float v) { m.delayFeedback = v; });
    setter (5,  [] (MasterSettings& m, float v) { m.delayLevel = v; });
    setter (6,  [] (MasterSettings& m, float v) { m.eqLowDb = v; });
    setter (7,  [] (MasterSettings& m, float v) { m.eqMidDb = v; });
    setter (8,  [] (MasterSettings& m, float v) { m.eqHighDb = v; });
    setter (9,  [] (MasterSettings& m, float v) { m.compThresholdDb = v; });
    setter (10, [] (MasterSettings& m, float v) { m.compRatio = v; });
    setter (11, [] (MasterSettings& m, float v) { m.masterDb = v; });

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
    const float vals[] = { m.reverbSize, m.reverbDamping, m.reverbLevel, m.delayTimeMs, m.delayFeedback, m.delayLevel,
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
    auto r = getLocalBounds().reduced (24, 20);
    auto row1 = r.removeFromTop (150).withTrimmedTop (40);
    const int kw = 110, gap = 30;
    int x = row1.getX();
    for (int si = 0; si < 3; ++si)
    {
        auto& s = sections.getReference (si);
        for (int k = 0; k < s.count; ++k, x += kw)
            knobs[s.first + k]->setBounds (x, row1.getY(), kw, 110);
        x += gap;
    }
    auto row2 = r.removeFromTop (190).withTrimmedTop (60);
    x = row2.getX();
    for (int si = 3; si < sections.size(); ++si)
    {
        auto& s = sections.getReference (si);
        for (int k = 0; k < s.count; ++k, x += kw)
            knobs[s.first + k]->setBounds (x, row2.getY(), kw, 110);
        x += gap;
    }
    globalVolume->setBounds (x + 80, row2.getY(), 160, 110);
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

#include "PluginEditor.h"
#include "MappingEditor.h"
#include "Text.h"

//==============================================================================
//  Look and feel
//==============================================================================
SamplerLookAndFeel::SamplerLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Pal::bg);
    setColour (juce::Label::textColourId, Pal::text);
    setColour (juce::TextButton::buttonColourId, Pal::panelHi);
    setColour (juce::TextButton::textColourOffId, Pal::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ComboBox::backgroundColourId, Pal::panelHi);
    setColour (juce::ComboBox::outlineColourId, Pal::panelHi);
    setColour (juce::ComboBox::textColourId, Pal::text);
    setColour (juce::PopupMenu::backgroundColourId, Pal::panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Pal::panelHi.brighter (0.2f));
    setColour (juce::Slider::textBoxTextColourId, Pal::dim);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId, Pal::text);
    setColour (juce::ToggleButton::tickColourId, Pal::part[0]);
}

void SamplerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                           float startAngle, float endAngle, juce::Slider& s)
{
    auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto c = bounds.getCentre();
    const float lineW = juce::jmax (3.0f, radius * 0.16f);
    const float arcR = radius - lineW * 0.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (Pal::panelHi.brighter (0.15f));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // bipolarni parametri (pan, transpozicija...) crtaju luk od sredine
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    juce::Path value;
    value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (s.isEnabled() ? accent : accent.withAlpha (0.3f));
    g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float knobR = arcR - lineW * 1.2f;
    g.setColour (Pal::panelHi);
    g.fillEllipse (c.x - knobR, c.y - knobR, knobR * 2.0f, knobR * 2.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -knobR, 3.0f, knobR * 0.55f, 1.5f);
    g.setColour (Pal::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void SamplerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                                               bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    auto col = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
    if (down) col = col.brighter (0.25f);
    else if (highlighted) col = col.brighter (0.1f);
    g.setColour (col);
    g.fillRoundedRectangle (r, 6.0f);
}

juce::Font SamplerLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (17.0f, buttonHeight * 0.5f), juce::Font::bold);
}

juce::Label* SamplerLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (juce::FontOptions (12.5f));
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    return l;
}

//==============================================================================
//  Knob
//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
            const juce::String& title, juce::Colour accent)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setMouseDragSensitivity (220);
    addAndMakeVisible (slider);

    label.setText (title, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    label.setColour (juce::Label::textColourId, Pal::text.withAlpha (0.85f));
    label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (label);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramID, slider);

    if (auto* p = apvts.getParameter (paramID))
        slider.setDoubleClickReturnValue (true, (double) p->convertFrom0to1 (p->getDefaultValue()));
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

//==============================================================================
//  PartPanel
//==============================================================================
PartPanel::PartPanel (LiveSamplerProcessor& p, int partIndex)
    : proc (p), part (partIndex), accent (Pal::part[partIndex])
{
    auto& apvts = proc.apvts;
    auto id = [this] (const char* n) { return LiveSamplerProcessor::pid (part, n); };

    onButton.setButtonText (LiveSamplerProcessor::partName (part));
    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonOnColourId, accent);
    onButton.setTooltip (T ("Uključi / isključi ovaj deo"));
    addAndMakeVisible (onButton);
    onAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, id ("on"), onButton);

    monoButton.setButtonText ("MONO");
    monoButton.setClickingTogglesState (true);
    monoButton.setColour (juce::TextButton::buttonOnColourId, accent);
    addAndMakeVisible (monoButton);
    monoAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, id ("mono"), monoButton);

    instrumentButton.setColour (juce::TextButton::buttonColourId, Pal::bg);
    instrumentButton.onClick = [this] { showInstrumentMenu(); };
    addAndMakeVisible (instrumentButton);
    prevButton.onClick = [this] { stepInstrument (-1); };
    nextButton.onClick = [this] { stepInstrument (+1); };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);

    fxLabel.setText ("Insert efekat", juce::dontSendNotification);
    fxLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    fxLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (fxLabel);
    fxBox.addItemList (InsertFx::getTypeNames(), 1);
    addAndMakeVisible (fxBox);
    fxAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, id ("fxtype"), fxBox);

    struct K { const char* id; const char* title; };
    const K list[] = {
        { "vol", "Jačina" }, { "pan", "Pan" }, { "oct", "Oktava" }, { "trans", "Transp." }, { "fine", "Fino" },
        { "velsens", "Velocity" }, { "porta", "Portamento" }, { "bend", "P.bend" }, { "rev", "Reverb" }, { "dly", "Delay" },
        { "cutoff", "Cutoff" }, { "reso", "Rezonanca" }, { "att", "Attack" }, { "dec", "Decay" }, { "sus", "Sustain" }, { "rel", "Release" },
        { "fxrate", "Brzina" }, { "fxdepth", "Dubina" }, { "fxmix", "Mix" } };

    for (auto& k : list)
        addAndMakeVisible (knobs.add (new Knob (apvts, id (k.id), T (k.title), accent)));

    refresh();
}

void PartPanel::refresh()
{
    auto name = proc.getInstrumentName (part);
    instrumentButton.setButtonText (name.isEmpty() ? T ("— izaberi instrument —") : name);
}

void PartPanel::showInstrumentMenu()
{
    const auto& names = proc.getInstrumentList();
    if (names.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Live Sampler",
            T ("Prvo izaberi folder sa semplovima (dugme gore levo)."));
        return;
    }

    // grupisi po prvom folderu: "Harmonike/Weltmeister" -> podmeni "Harmonike"
    juce::PopupMenu menu;
    std::map<juce::String, juce::PopupMenu> groups;
    juce::StringArray groupOrder;
    const auto current = proc.getInstrumentName (part);

    for (int i = 0; i < names.size(); ++i)
    {
        const auto& n = names[i];
        const int slash = n.indexOfChar ('/');
        if (slash > 0)
        {
            auto group = n.substring (0, slash);
            if (! groupOrder.contains (group)) groupOrder.add (group);
            groups[group].addItem (i + 1, n.substring (slash + 1), true, n == current);
        }
        else
        {
            menu.addItem (i + 1, n, true, n == current);
        }
    }
    for (auto& g : groupOrder)
        menu.addSubMenu (g, groups[g], true, nullptr, current.startsWith (g + "/"));

    menu.addSeparator();
    menu.addItem (100000, T ("(prazno)"));

    juce::Component::SafePointer<PartPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&instrumentButton).withMinimumWidth (260),
        [safe] (int result)
        {
            if (safe == nullptr || result == 0) return;
            auto& list = safe->proc.getInstrumentList();
            if (result == 100000)                         safe->proc.selectInstrument (safe->part, {});
            else if (result - 1 < list.size())            safe->proc.selectInstrument (safe->part, list[result - 1]);
        });
}

void PartPanel::stepInstrument (int delta)
{
    const auto& names = proc.getInstrumentList();
    if (names.isEmpty())
        return;
    int idx = names.indexOf (proc.getInstrumentName (part));
    idx = idx < 0 ? 0 : (idx + delta + names.size()) % names.size();
    proc.selectInstrument (part, names[idx]);
}

void PartPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (r.removeFromTop (4.0f).reduced (10.0f, 0.0f), 2.0f);
}

void PartPanel::resized()
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (4);

    auto header = r.removeFromTop (38);
    monoButton.setBounds (header.removeFromRight (80));
    header.removeFromRight (8);
    onButton.setBounds (header);

    r.removeFromTop (8);
    auto instRow = r.removeFromTop (46);
    prevButton.setBounds (instRow.removeFromLeft (40));
    nextButton.setBounds (instRow.removeFromRight (40));
    instRow.reduce (6, 0);
    instrumentButton.setBounds (instRow);
    r.removeFromTop (8);

    const int cols = 6;
    const int colW = r.getWidth() / cols;
    const int rowH = r.getHeight() / 4;

    auto placeRow = [&] (int firstKnob, int count, int startSlot, juce::Rectangle<int> row)
    {
        for (int i = 0; i < count; ++i)
            knobs[firstKnob + i]->setBounds (row.getX() + (startSlot + i) * colW, row.getY(), colW, row.getHeight());
    };

    auto centredRow = [&] (int count)
    {
        auto row = r.removeFromTop (rowH);
        const int w = count * colW;
        return row.withX (row.getX() + (row.getWidth() - w) / 2).withWidth (w);
    };

    placeRow (0, 5, 0, centredRow (5));     // jacina, pan, oktava, transp, fino
    placeRow (5, 5, 0, centredRow (5));     // velocity, portamento, bend, reverb, delay
    placeRow (10, 6, 0, r.removeFromTop (rowH)); // filter + ADSR

    auto fxRow = r.removeFromTop (rowH);
    auto fxArea = fxRow.removeFromLeft (colW * 3).reduced (6, 0);
    fxLabel.setBounds (fxArea.removeFromTop (16));
    fxBox.setBounds (fxArea.withSizeKeepingCentre (fxArea.getWidth(), 32));
    placeRow (16, 3, 0, fxRow);
}

//==============================================================================
//  MasterPanel
//==============================================================================
MasterPanel::MasterPanel (LiveSamplerProcessor& p)
{
    auto& apvts = p.apvts;
    const auto acc = Pal::master;

    splitButton.setButtonText ("SPLIT");
    splitButton.setClickingTogglesState (true);
    splitButton.setColour (juce::TextButton::buttonOnColourId, Pal::part[2]);
    addAndMakeVisible (splitButton);
    splitAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "split_on", splitButton);

    struct K { const char* id; const char* title; };
    auto add = [&] (std::initializer_list<K> ks, const char* sectionTitle, juce::Colour col)
    {
        sections.add ({ T (sectionTitle), knobs.size(), (int) ks.size() });
        for (auto& k : ks)
            addAndMakeVisible (knobs.add (new Knob (apvts, k.id, T (k.title), col)));
    };

    add ({ { "split_pt", "Tačka" }, { "lower_ch", "Donji kanal" } }, "SPLIT", Pal::part[2]);
    add ({ { "rev_size", "Veličina" }, { "rev_damp", "Prigušenje" }, { "rev_level", "Nivo" } }, "REVERB", acc);
    add ({ { "dly_time", "Vreme" }, { "dly_fb", "Ponavljanje" }, { "dly_level", "Nivo" } }, "DELAY", acc);
    add ({ { "eq_low", "Bas" }, { "eq_mid", "Sredina" }, { "eq_high", "Visoki" } }, "EQ", acc);
    add ({ { "comp_thr", "Prag" }, { "comp_ratio", "Odnos" } }, "KOMPRESOR", acc);
    add ({ { "master_vol", "Jačina" } }, "MASTER", juce::Colours::white);
}

void MasterPanel::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);

    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    for (auto& s : sections)
    {
        auto first = knobs[s.first]->getBounds();
        auto last = knobs[s.first + s.count - 1]->getBounds();
        int x0 = first.getX();
        if (s.first == 0)
            x0 = splitButton.getX();
        auto titleArea = juce::Rectangle<int> (x0, 6, last.getRight() - x0, 18);
        g.setColour (Pal::dim);
        g.drawText (s.title, titleArea, juce::Justification::centred);
        g.setColour (Pal::panelHi.brighter (0.1f));
        g.drawLine ((float) x0 + 4, 26.0f, (float) last.getRight() - 4, 26.0f, 1.0f);
    }
}

void MasterPanel::resized()
{
    auto r = getLocalBounds().reduced (10, 0);
    r.removeFromTop (30);
    r.removeFromBottom (8);

    const int gap = 18;
    const int totalSlots = knobs.size() + 1; // +1 za split dugme
    const int slotW = (r.getWidth() - gap * (sections.size() - 1)) / totalSlots;

    int x = r.getX();
    for (int si = 0; si < sections.size(); ++si)
    {
        auto& s = sections.getReference (si);
        if (si == 0)
        {
            splitButton.setBounds (juce::Rectangle<int> (x, r.getY(), slotW, r.getHeight()).withSizeKeepingCentre (slotW - 8, 40));
            x += slotW;
        }
        for (int k = 0; k < s.count; ++k)
        {
            knobs[s.first + k]->setBounds (x, r.getY(), slotW, r.getHeight());
            x += slotW;
        }
        x += gap;
    }
}

//==============================================================================
//  RegisterBar
//==============================================================================
RegisterBar::RegisterBar (LiveSamplerProcessor& p) : proc (p)
{
    const char* bankNames[] = { "A", "B", "C", "D" };
    for (int b = 0; b < Registers::numBanks; ++b)
    {
        bankButtons[b].setButtonText (bankNames[b]);
        bankButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::master);
        bankButtons[b].onClick = [this, b] { bank = b; refresh(); };
        addAndMakeVisible (bankButtons[b]);
    }

    for (int i = 0; i < Registers::perBank; ++i)
    {
        regButtons[i].setButtonText (juce::String (i + 1));
        regButtons[i].onClick = [this, i] { registerClicked (i); };
        addAndMakeVisible (regButtons[i]);
    }

    saveButton.setButtonText ("SNIMI");
    saveButton.setClickingTogglesState (true);
    saveButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffff4d5e));
    saveButton.setTooltip (T ("Uključi SNIMI pa klikni registar da sačuvaš trenutno stanje"));
    saveButton.onClick = [this] { refresh(); };
    addAndMakeVisible (saveButton);

    refresh();
}

void RegisterBar::registerClicked (int i)
{
    const int index = bank * Registers::perBank + i;
    if (saveButton.getToggleState())
    {
        proc.storeRegister (index);
        saveButton.setToggleState (false, juce::dontSendNotification);
    }
    else
    {
        proc.recallRegister (index);
    }
    refresh();
}

void RegisterBar::refresh()
{
    const bool saving = saveButton.getToggleState();
    for (int b = 0; b < Registers::numBanks; ++b)
        bankButtons[b].setToggleState (b == bank, juce::dontSendNotification);

    for (int i = 0; i < Registers::perBank; ++i)
    {
        const int index = bank * Registers::perBank + i;
        const bool used = proc.getRegisters()[index].used;
        const bool current = proc.getCurrentRegister() == index;

        auto col = current ? Pal::master
                 : used    ? Pal::panelHi.brighter (0.35f)
                           : Pal::panelHi.darker (0.3f);
        if (saving) col = col.interpolatedWith (juce::Colour (0xffff4d5e), 0.35f);

        regButtons[i].setColour (juce::TextButton::buttonColourId, col);
        regButtons[i].setColour (juce::TextButton::textColourOffId, current ? juce::Colours::black
                                                                    : (used ? Pal::text : Pal::dim));
    }
    repaint();
}

void RegisterBar::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    g.setColour (Pal::dim);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("REGISTRI", getLocalBounds().reduced (14, 0).removeFromLeft (90), juce::Justification::centredLeft);
}

void RegisterBar::resized()
{
    auto r = getLocalBounds().reduced (10, 9);
    r.removeFromLeft (90);

    for (auto& b : bankButtons)
    {
        b.setBounds (r.removeFromLeft (46));
        r.removeFromLeft (4);
    }
    r.removeFromLeft (16);

    saveButton.setBounds (r.removeFromRight (120));
    r.removeFromRight (16);

    const int w = r.getWidth() / Registers::perBank;
    for (auto& b : regButtons)
        b.setBounds (r.removeFromLeft (w).reduced (3, 0));
}

//==============================================================================
//  Editor
//==============================================================================
void LiveSamplerEditor::Content::paint (juce::Graphics& g)
{
    g.fillAll (Pal::bg);
}

LiveSamplerEditor::LiveSamplerEditor (LiveSamplerProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      masterPanel (p), registerBar (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);
    content.setSize (designW, designH);

    title.setText ("LIVE SAMPLER", juce::dontSendNotification);
    title.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, Pal::part[0]);
    content.addAndMakeVisible (title);

    folderButton.setButtonText ("Folder sa semplovima...");
    folderButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (T ("Izaberi glavni folder sa semplovima"),
                                                       proc.getLibraryFolder(), "*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.isDirectory())
                    proc.setLibraryFolder (f);
            });
    };
    content.addAndMakeVisible (folderButton);

    status.setFont (juce::FontOptions (14.0f));
    status.setColour (juce::Label::textColourId, Pal::dim);
    content.addAndMakeVisible (status);

    c3Toggle.setButtonText ("C3 = MIDI 60");
    c3Toggle.setTooltip (T ("Uključeno: C3 u nazivu fajla je srednje C (Yamaha/Korg). Isključeno: C4 je srednje C."));
    c3Toggle.setToggleState (proc.getC3Is60(), juce::dontSendNotification);
    c3Toggle.onClick = [this]
    {
        proc.setC3Is60 (c3Toggle.getToggleState());
        keyboard.setOctaveForMiddleC (proc.getC3Is60() ? 3 : 4);
    };
    content.addAndMakeVisible (c3Toggle);

    panicButton.setButtonText ("PANIK");
    panicButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7a2230));
    panicButton.setTooltip (T ("Odmah utišaj sve note"));
    panicButton.onClick = [this] { proc.panic(); };
    content.addAndMakeVisible (panicButton);

    mappingButton.setButtonText ("MAPIRANJE");
    mappingButton.setClickingTogglesState (true);
    mappingButton.setColour (juce::TextButton::buttonOnColourId, Pal::master);
    mappingButton.setTooltip (T ("Editor mapiranja: koji sempl svira na kojim tasterima i jačinama"));
    mappingButton.onClick = [this] { showMapping (mappingButton.getToggleState()); };
    content.addAndMakeVisible (mappingButton);

    for (int i = 0; i < LiveSamplerProcessor::numParts; ++i)
    {
        partPanels[i] = std::make_unique<PartPanel> (proc, i);
        content.addAndMakeVisible (*partPanels[i]);
    }

    content.addAndMakeVisible (masterPanel);
    content.addAndMakeVisible (registerBar);

    keyboard.setAvailableRange (21, 108);
    keyboard.setOctaveForMiddleC (proc.getC3Is60() ? 3 : 4);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, Pal::part[0].withAlpha (0.7f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, Pal::part[1].withAlpha (0.3f));
    keyboard.setWantsKeyboardFocus (false);
    content.addAndMakeVisible (keyboard);

    mappingEditor = std::make_unique<MappingEditor> (proc);
    mappingEditor->onClose = [this] { showMapping (false); };
    content.addChildComponent (*mappingEditor);

    setResizable (true, true);
    setResizeLimits (640, 640 * designH / designW, 2560, 2560 * designH / designW);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) designW / designH);
    setSize (1180, 1180 * designH / designW);

    proc.addChangeListener (this);
    refreshAll();
    startTimer (400);
}

LiveSamplerEditor::~LiveSamplerEditor()
{
    proc.removeChangeListener (this);
    mappingEditor.reset();
    setLookAndFeel (nullptr);
}

void LiveSamplerEditor::showMapping (bool show)
{
    mappingButton.setToggleState (show, juce::dontSendNotification);
    mappingEditor->setVisible (show);
    if (show)
    {
        mappingEditor->refresh();
        mappingEditor->toFront (true);
    }
}

void LiveSamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (Pal::bg);
}

void LiveSamplerEditor::resized()
{
    const float scale = (float) getWidth() / (float) designW;
    content.setTransform (juce::AffineTransform::scale (scale));

    // raspored u "dizajn" koordinatama (1280 x 860)
    auto r = juce::Rectangle<int> (0, 0, designW, designH).reduced (12);

    auto top = r.removeFromTop (44);
    title.setBounds (top.removeFromLeft (200));
    folderButton.setBounds (top.removeFromLeft (210).reduced (0, 2));
    top.removeFromLeft (10);
    mappingButton.setBounds (top.removeFromLeft (130).reduced (0, 2));
    top.removeFromLeft (14);
    panicButton.setBounds (top.removeFromRight (100).reduced (0, 2));
    top.removeFromRight (14);
    c3Toggle.setBounds (top.removeFromRight (140));
    status.setBounds (top);

    r.removeFromTop (10);
    mappingEditor->setBounds (r.getX(), r.getY(), r.getWidth(), 440 + 12 + 138 + 12 + 58);
    auto partsArea = r.removeFromTop (440);
    const int pw = (partsArea.getWidth() - 2 * 12) / 3;
    for (int i = 0; i < LiveSamplerProcessor::numParts; ++i)
    {
        partPanels[i]->setBounds (partsArea.removeFromLeft (pw));
        partsArea.removeFromLeft (12);
    }

    r.removeFromTop (12);
    masterPanel.setBounds (r.removeFromTop (138));
    r.removeFromTop (12);
    registerBar.setBounds (r.removeFromTop (58));
    r.removeFromTop (12);
    keyboard.setBounds (r);
    keyboard.setKeyWidth ((float) r.getWidth() / 52.0f);
}

void LiveSamplerEditor::refreshAll()
{
    for (auto& pnl : partPanels)
        pnl->refresh();
    registerBar.refresh();
    if (mappingEditor->isVisible())
        mappingEditor->refresh();
    status.setText (proc.getStatusText(), juce::dontSendNotification);
    if (c3Toggle.getToggleState() != proc.getC3Is60())
        c3Toggle.setToggleState (proc.getC3Is60(), juce::dontSendNotification);
}

void LiveSamplerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
}

void LiveSamplerEditor::timerCallback()
{
    status.setText (proc.getStatusText(), juce::dontSendNotification);
}

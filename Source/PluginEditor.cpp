#include "PluginEditor.h"
#include "Pages.h"
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
    setColour (juce::ComboBox::arrowColourId, Pal::dim);
    setColour (juce::PopupMenu::backgroundColourId, Pal::panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Pal::panelHi.brighter (0.2f));
    setColour (juce::Slider::textBoxTextColourId, Pal::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, Pal::bg);
    setColour (juce::ToggleButton::textColourId, Pal::text);
    setColour (juce::ToggleButton::tickColourId, Pal::accent);
    setColour (juce::ListBox::backgroundColourId, Pal::bg);
    setColour (juce::TextEditor::backgroundColourId, Pal::bg);
    setColour (juce::TextEditor::textColourId, Pal::text);
    setColour (juce::TextEditor::highlightColourId, Pal::master.withAlpha (0.4f));
    setColour (juce::CaretComponent::caretColourId, Pal::text);
    setColour (juce::ScrollBar::thumbColourId, Pal::panelHi.brighter (0.3f));
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

    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle) : startAngle;
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
    if (! b.isEnabled()) col = col.withMultipliedAlpha (0.4f);
    if (down) col = col.brighter (0.25f);
    else if (highlighted) col = col.brighter (0.1f);
    g.setColour (col);
    g.fillRoundedRectangle (r, 6.0f);
}

juce::Font SamplerLookAndFeel::getTextButtonFont (juce::TextButton& b, int buttonHeight)
{
    if (b.getProperties().contains ("fontSize"))
        return juce::FontOptions ((float) b.getProperties()["fontSize"], juce::Font::bold);
    return juce::FontOptions (juce::jmin (16.0f, buttonHeight * 0.5f), juce::Font::bold);
}

juce::Label* SamplerLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (juce::FontOptions (13.0f));
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundColourId,
                  s.getSliderStyle() == juce::Slider::IncDecButtons ? Pal::bg : juce::Colours::transparentBlack);
    return l;
}

//==============================================================================
//  UI pomocne funkcije
//==============================================================================
namespace UI
{
    juce::String noteName (int n, bool c3Is60)
    {
        return juce::MidiMessage::getMidiNoteName (n, true, true, c3Is60 ? 3 : 4);
    }

    void setupIncDec (juce::Slider& s, double lo, double hi, double step,
                      std::function<juce::String (double)> fmt, std::function<double (const juce::String&)> parse)
    {
        s.setSliderStyle (juce::Slider::IncDecButtons);
        s.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 22);
        s.setRange (lo, hi, step);
        if (fmt) s.textFromValueFunction = fmt;
        if (parse) s.valueFromTextFunction = parse;
        s.updateText();
    }

    void setupNoteBox (juce::Slider& s, std::function<bool()> c3Is60)
    {
        setupIncDec (s, 0, 127, 1,
            [c3Is60] (double v) { return noteName ((int) v, c3Is60()); },
            [c3Is60] (const juce::String& t)
            {
                auto txt = t.trim();
                if (txt.containsOnly ("0123456789"))
                    return (double) txt.getIntValue();
                int note, layer;
                if (SampleLibrary::parseFileName (txt, c3Is60(), note, layer))
                    return (double) note;
                return (double) txt.getIntValue();
            });
    }

    void sectionTitle (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour c)
    {
        g.setColour (c);
        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.drawText (text, r, juce::Justification::centredLeft);
        g.setColour (Pal::panelHi.brighter (0.1f));
        g.drawHorizontalLine (r.getBottom(), (float) r.getX(), (float) r.getRight());
    }

    juce::String dbText (double v)  { return v <= -59.5 ? juce::String ("-inf") : juce::String (v, 1) + " dB"; }
    juce::String panText (double v)
    {
        const int x = juce::roundToInt (v * 50.0);
        return x == 0 ? juce::String ("C") : (x < 0 ? "L" + juce::String (-x) : "R" + juce::String (x));
    }
    juce::String timeText (double v) { return v < 1.0 ? juce::String (juce::roundToInt (v * 1000.0)) + " ms" : juce::String (v, 2) + " s"; }
    juce::String hzText (double v)   { return v < 1000.0 ? juce::String (juce::roundToInt (v)) + " Hz" : juce::String (v / 1000.0, 1) + " kHz"; }
    juce::String pctText (double v)  { return juce::String (juce::roundToInt (v * 100.0)) + "%"; }

    void showInstrumentMenu (juce::Component* target, const juce::StringArray& names, const juce::String& current,
                             bool allowEmpty, std::function<void (const juce::String&)> onPick)
    {
        if (names.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Live Sampler",
                T ("Nema instrumenata. Dodaj folder sa semplovima u PODEŠAVANJIMA."));
            return;
        }

        // stablo menija po folderima (do 3 nivoa)
        struct Node { std::map<juce::String, Node> children; std::vector<std::pair<juce::String, int>> items; };
        Node rootNode;
        for (int i = 0; i < names.size(); ++i)
        {
            auto partsArr = juce::StringArray::fromTokens (names[i], "/", {});
            Node* node = &rootNode;
            const int depth = juce::jmin (partsArr.size() - 1, 3);
            for (int d = 0; d < depth; ++d)
                node = &node->children[partsArr[d]];
            juce::StringArray rest;
            for (int d = depth; d < partsArr.size(); ++d) rest.add (partsArr[d]);
            node->items.push_back ({ rest.joinIntoString ("/"), i });
        }

        std::function<juce::PopupMenu (const Node&)> build = [&] (const Node& n)
        {
            juce::PopupMenu m;
            for (auto& kv : n.children)
            {
                bool containsCurrent = false;
                for (auto& it : kv.second.items) if (names[it.second] == current) containsCurrent = true;
                m.addSubMenu (kv.first, build (kv.second), true, nullptr, containsCurrent || current.contains ("/" + kv.first + "/")
                                                                                         || current.startsWith (kv.first + "/"));
            }
            for (auto& it : n.items)
                m.addItem (it.second + 1, it.first, true, names[it.second] == current);
            return m;
        };

        auto menu = build (rootNode);
        if (allowEmpty)
        {
            menu.addSeparator();
            menu.addItem (100000, T ("(prazno)"));
        }

        auto namesCopy = names;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMinimumWidth (280),
            [namesCopy, onPick] (int result)
            {
                if (result == 0) return;
                if (result == 100000) onPick ({});
                else if (result - 1 < namesCopy.size()) onPick (namesCopy[result - 1]);
            });
    }
}

//==============================================================================
//  ValueKnob
//==============================================================================
ValueKnob::ValueKnob (const juce::String& title, juce::Colour accent, double lo, double hi, double centre,
                      std::function<juce::String (double)> fmt, double defaultValue)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.setColour (juce::Slider::textBoxTextColourId, Pal::dim);
    slider.setMouseDragSensitivity (220);
    slider.setRange (lo, hi, 0.0);
    if (centre > lo && centre < hi)
        slider.setSkewFactorFromMidPoint (centre);
    if (fmt)
        slider.textFromValueFunction = fmt;
    slider.setDoubleClickReturnValue (true, defaultValue);
    slider.setValue (defaultValue, juce::dontSendNotification);
    slider.updateText();
    slider.onValueChange = [this] { if (onChange) onChange (slider.getValue()); };
    addAndMakeVisible (slider);

    label.setText (title, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    label.setColour (juce::Label::textColourId, Pal::text.withAlpha (0.85f));
    label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (label);
}

void ValueKnob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (15));
    slider.setBounds (r);
}

//==============================================================================
//  Editor
//==============================================================================
void LiveSamplerEditor::Content::paint (juce::Graphics& g)
{
    g.fillAll (Pal::bg);
    // traka sa tabovima
    g.setColour (Pal::panel);
    g.fillRect (0, 0, designW, 120);
    g.setColour (Pal::panelHi);
    g.drawHorizontalLine (120, 0.0f, (float) designW);
}

void LiveSamplerEditor::Display::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    juce::ColourGradient grad (Pal::lcdBg.brighter (0.15f), 0, 0, Pal::lcdBg, 0, r.getHeight(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (Pal::lcdText.withAlpha (0.25f));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    const auto& perf = proc->getPerformance();
    const int slot = proc->getCurrentSlot();
    auto inner = getLocalBounds().reduced (12, 6);

    auto left = inner.removeFromLeft (64);
    g.setColour (Pal::lcdText.withAlpha (0.6f));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("PERFORM", left.removeFromTop (16), juce::Justification::centredLeft);
    g.setColour (Pal::lcdText);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    const juce::String slotText = slot >= 0 ? juce::String::charToString ((juce::juce_wchar) ('A' + slot / kPerfPerBank))
                                                  + juce::String (slot % kPerfPerBank + 1).paddedLeft ('0', 2)
                                            : juce::String ("---");
    g.drawText (slotText, left, juce::Justification::centredLeft);

    auto top = inner.removeFromTop (34);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText (perf.name + (proc->isPerformanceDirty() ? " *" : ""), top, juce::Justification::centredLeft, true);

    // aktivni delovi
    int x = inner.getX();
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    for (int i = 0; i < kNumParts; ++i)
    {
        const auto& p = perf.parts[(size_t) i];
        if (! p.on || p.user.isEmpty()) continue;
        const auto txt = p.user;
        const int w = juce::jmin (130, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (12.0f, juce::Font::bold), txt) + 14);
        if (x + w > inner.getRight()) break;
        auto chip = juce::Rectangle<int> (x, inner.getY() + 2, w, 18);
        g.setColour (Pal::part[i].withAlpha (0.25f));
        g.fillRoundedRectangle (chip.toFloat(), 4.0f);
        g.setColour (Pal::part[i]);
        g.drawText (txt, chip.reduced (6, 0), juce::Justification::centredLeft, true);
        x += w + 6;
    }
}

void LiveSamplerEditor::Display::mouseUp (const juce::MouseEvent&)
{
    if (onClick) onClick();
}

void LiveSamplerEditor::TercaButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = proc->isTercaOn();
    const auto green = juce::Colour (0xff3ddc84);
    g.setColour (on ? green.withAlpha (isMouseButtonDown() ? 0.45f : 0.28f) : Pal::panelHi.withAlpha (isMouseButtonDown() ? 1.0f : 0.8f));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (on ? green : Pal::dim);
    g.drawRoundedRectangle (r, 8.0f, on ? 2.0f : 1.0f);

    auto inner = getLocalBounds().reduced (10, 6);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (on ? "TERCA ON" : "TERCA OFF", inner.removeFromTop (16), juce::Justification::centredLeft);
    g.setColour (on ? Pal::text : Pal::dim);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText (proc->getTercaText(), inner, juce::Justification::centredLeft);
}

LiveSamplerEditor::LiveSamplerEditor (LiveSamplerProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lnf);
    content.owner = this;
    addAndMakeVisible (content);
    content.setSize (designW, designH);

    logo.setText ("LIVE\nSAMPLER", juce::dontSendNotification);
    logo.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    logo.setColour (juce::Label::textColourId, Pal::accent);
    content.addAndMakeVisible (logo);

    display.proc = &proc;
    display.onClick = [this] { showTab (TabPerformance); };
    content.addAndMakeVisible (display);

    tercaButton.proc = &proc;
    tercaButton.setTooltip (T ("Drži i odsviraj akord: terce u tom tonalitetu. Kratak klik: pali/gasi terce."));
    content.addAndMakeVisible (tercaButton);

    learnButton.setButtonText ("LEARN");
    learnButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb340));
    learnButton.setTooltip (T ("Dodeli dugme sa klavijature (npr. Impulse) za terce: klikni LEARN pa pritisni dugme."));
    learnButton.onClick = [this] { proc.startLearn (! proc.isLearning()); };
    content.addAndMakeVisible (learnButton);

    status.setFont (juce::FontOptions (13.0f));
    status.setColour (juce::Label::textColourId, Pal::dim);
    status.setJustificationType (juce::Justification::centredRight);
    content.addAndMakeVisible (status);

    panicButton.setButtonText ("PANIK");
    panicButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7a2230));
    panicButton.onClick = [this] { proc.panic(); };
    content.addAndMakeVisible (panicButton);

    const char* tabNames[] = { "PERFORMANS", "USER", "MAPIRANJE", "TERCE", "EFEKTI", "PODEŠAVANJA" };
    for (int i = 0; i < NumTabs; ++i)
    {
        tabButtons[i].setButtonText (T (tabNames[i]));
        tabButtons[i].setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        tabButtons[i].setColour (juce::TextButton::buttonColourId, Pal::bg);
        tabButtons[i].onClick = [this, i] { showTab (i); };
        content.addAndMakeVisible (tabButtons[i]);
    }

    performancePage = std::make_unique<PerformancePage> (proc, *this);
    userPage = std::make_unique<UserPage> (proc, *this);
    mappingEditor = std::make_unique<MappingEditor> (proc);
    tercePage = std::make_unique<TercePage> (proc);
    effectsPage = std::make_unique<EffectsPage> (proc);
    settingsPage = std::make_unique<SettingsPage> (proc);
    mappingEditor->onClose = [this] { showTab (TabUser); };
    for (auto* c : std::initializer_list<juce::Component*> { performancePage.get(), userPage.get(), mappingEditor.get(),
                                                             tercePage.get(), effectsPage.get(), settingsPage.get() })
        content.addChildComponent (c);

    for (int b = 0; b < kNumPerfBanks; ++b)
    {
        bankButtons[b].setButtonText (juce::String::charToString ((juce::juce_wchar) ('A' + b)));
        bankButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::lcdText);
        bankButtons[b].onClick = [this, b] { proc.setCurrentBank (b); };
        content.addAndMakeVisible (bankButtons[b]);
    }
    for (int i = 0; i < kPerfPerBank; ++i)
    {
        slotButtons[i].getProperties().set ("fontSize", 12.0f);
        slotButtons[i].onClick = [this, i]
        {
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
            refreshSlots();
        };
        content.addAndMakeVisible (slotButtons[i]);
    }
    writeButton.setButtonText ("SNIMI");
    writeButton.setClickingTogglesState (true);
    writeButton.setColour (juce::TextButton::buttonOnColourId, Pal::red);
    writeButton.setTooltip (T ("Uključi SNIMI pa klikni mesto da sačuvaš trenutni performans"));
    writeButton.onClick = [this] { refreshSlots(); };
    content.addAndMakeVisible (writeButton);

    keyboard.setAvailableRange (21, 108);
    keyboard.setOctaveForMiddleC (proc.getC3Is60() ? 3 : 4);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, Pal::accent.withAlpha (0.7f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, Pal::part[1].withAlpha (0.3f));
    keyboard.setWantsKeyboardFocus (false);
    content.addAndMakeVisible (keyboard);

    setResizable (true, true);
    setResizeLimits (640, 640 * designH / designW, 2560, 2560 * designH / designW);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) designW / designH);
    setSize (1200, 1200 * designH / designW);

    proc.addChangeListener (this);
    showTab (TabPerformance);
    refreshAll();
    startTimerHz (10);
}

LiveSamplerEditor::~LiveSamplerEditor()
{
    proc.removeChangeListener (this);
    performancePage.reset();
    userPage.reset();
    mappingEditor.reset();
    tercePage.reset();
    effectsPage.reset();
    settingsPage.reset();
    setLookAndFeel (nullptr);
}

void LiveSamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (Pal::bg);
}

void LiveSamplerEditor::showTab (int tab)
{
    currentTab = juce::jlimit (0, NumTabs - 1, tab);
    juce::Component* pages[] = { performancePage.get(), userPage.get(), mappingEditor.get(),
                                 tercePage.get(), effectsPage.get(), settingsPage.get() };
    for (int i = 0; i < NumTabs; ++i)
    {
        pages[i]->setVisible (i == currentTab);
        tabButtons[i].setToggleState (i == currentTab, juce::dontSendNotification);
    }
    refreshAll();
}

void LiveSamplerEditor::editUser (const juce::String& userName)
{
    const int u = proc.findUser (userName);
    if (u >= 0)
        userPage->selectUser (u);
    showTab (TabUser);
}

void LiveSamplerEditor::editMapping (const juce::String& userName, int osc)
{
    const int u = proc.findUser (userName);
    if (u >= 0)
        mappingEditor->setUserOsc (u, osc);
    showTab (TabMapping);
}

void LiveSamplerEditor::resized()
{
    const float scale = (float) getWidth() / (float) designW;
    content.setTransform (juce::AffineTransform::scale (scale));

    auto r = juce::Rectangle<int> (0, 0, designW, designH);

    auto top = r.removeFromTop (76).reduced (12, 8);
    logo.setBounds (top.removeFromLeft (110));
    display.setBounds (top.removeFromLeft (520));
    top.removeFromLeft (10);
    tercaButton.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (6);
    learnButton.setBounds (top.removeFromLeft (80).reduced (0, 8));
    top.removeFromLeft (10);
    panicButton.setBounds (top.removeFromRight (90).reduced (0, 8));
    top.removeFromRight (10);
    status.setBounds (top);

    auto tabs = r.removeFromTop (44).reduced (12, 4);
    const int tw = tabs.getWidth() / NumTabs;
    for (auto& t : tabButtons)
        t.setBounds (tabs.removeFromLeft (tw).reduced (3, 0));

    auto kb = r.removeFromBottom (108).reduced (12, 0).withTrimmedBottom (8);
    keyboard.setBounds (kb);
    keyboard.setKeyWidth ((float) kb.getWidth() / 52.0f);

    auto slotsBar = r.removeFromBottom (58).reduced (12, 6);
    for (auto& b : bankButtons)
    {
        b.setBounds (slotsBar.removeFromLeft (40));
        slotsBar.removeFromLeft (4);
    }
    slotsBar.removeFromLeft (8);
    writeButton.setBounds (slotsBar.removeFromRight (90));
    slotsBar.removeFromRight (8);
    const int sw = slotsBar.getWidth() / kPerfPerBank;
    for (auto& b : slotButtons)
        b.setBounds (slotsBar.removeFromLeft (sw).reduced (2, 0));

    auto page = r.reduced (12, 6);
    for (auto* c : std::initializer_list<juce::Component*> { performancePage.get(), userPage.get(), mappingEditor.get(),
                                                             tercePage.get(), effectsPage.get(), settingsPage.get() })
        c->setBounds (page);
}

void LiveSamplerEditor::refreshSlots()
{
    const int bank = proc.getCurrentBank();
    const bool writing = writeButton.getToggleState();
    for (int b = 0; b < kNumPerfBanks; ++b)
        bankButtons[b].setToggleState (b == bank, juce::dontSendNotification);

    for (int i = 0; i < kPerfPerBank; ++i)
    {
        const int slot = bank * kPerfPerBank + i;
        const auto& s = proc.getSlot (slot);
        const bool current = proc.getCurrentSlot() == slot;
        slotButtons[i].setButtonText (juce::String (i + 1) + (s.used ? "\n" + s.name : juce::String()));
        auto col = current ? Pal::lcdText : (s.used ? Pal::panelHi.brighter (0.25f) : Pal::panelHi.darker (0.3f));
        if (writing) col = col.interpolatedWith (Pal::red, 0.35f);
        slotButtons[i].setColour (juce::TextButton::buttonColourId, col);
        slotButtons[i].setColour (juce::TextButton::textColourOffId, current ? juce::Colours::black : (s.used ? Pal::text : Pal::dim));
    }
}

void LiveSamplerEditor::refreshAll()
{
    display.repaint();
    tercaButton.repaint();
    learnButton.setToggleState (proc.isLearning(), juce::dontSendNotification);
    learnButton.setButtonText (proc.isLearning() ? T ("PRITISNI...") : "LEARN");
    status.setText (proc.getStatusText() + "  |  " + juce::String (proc.getActiveVoices()) + T (" glasova"), juce::dontSendNotification);
    keyboard.setOctaveForMiddleC (proc.getC3Is60() ? 3 : 4);
    refreshSlots();

    switch (currentTab)
    {
        case TabPerformance: performancePage->refresh(); break;
        case TabUser:        userPage->refresh(); break;
        case TabMapping:     mappingEditor->refresh(); break;
        case TabTerce:       tercePage->refresh(); break;
        case TabEffects:     effectsPage->refresh(); break;
        case TabSettings:    settingsPage->refresh(); break;
        default: break;
    }
}

void LiveSamplerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
}

void LiveSamplerEditor::timerCallback()
{
    status.setText (proc.getStatusText() + "  |  " + juce::String (proc.getActiveVoices()) + T (" glasova"), juce::dontSendNotification);
    tercaButton.repaint();
    learnButton.setToggleState (proc.isLearning(), juce::dontSendNotification);
    learnButton.setButtonText (proc.isLearning() ? T ("PRITISNI...") : "LEARN");
}

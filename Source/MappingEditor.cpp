#include "MappingEditor.h"
#include "PluginEditor.h"
#include "Text.h"

//==============================================================================
//  ZoneMapView
//==============================================================================
ZoneMapView::ZoneMapView (juce::MidiKeyboardState& ks) : keyboard (ks)
{
    startTimerHz (30);
}

juce::Colour ZoneMapView::colourFor (int sampleIndex)
{
    const float hue = std::fmod ((float) sampleIndex * 0.61803f + 0.08f, 1.0f);
    return juce::Colour::fromHSV (hue, 0.55f, 0.92f, 1.0f);
}

void ZoneMapView::setZones (const std::vector<Zone>& z, int selectedIndex, bool c3Is60)
{
    if (mode == None)
        zones = z;
    selected = selectedIndex;
    c3 = c3Is60;
    repaint();
}

void ZoneMapView::timerCallback()
{
    bool changed = false;
    for (int n = 0; n < 128; ++n)
    {
        const bool on = keyboard.isNoteOnForChannels (0xffff, n);
        if (on != held[(size_t) n]) { held[(size_t) n] = on; changed = true; }
    }
    if (changed)
        repaint();
}

juce::Rectangle<float> ZoneMapView::plotArea() const
{
    auto r = getLocalBounds().toFloat().reduced (6.0f);
    r.removeFromLeft (26.0f);     // oznake jacine
    r.removeFromBottom (18.0f);   // oznake nota
    return r;
}

juce::Rectangle<float> ZoneMapView::zoneRect (const Zone& z) const
{
    auto p = plotArea();
    const float kw = p.getWidth() / 128.0f;
    const float rh = p.getHeight() / 127.0f;
    return { p.getX() + (float) z.lowKey * kw,
             p.getY() + (float) (127 - z.highVel) * rh,
             (float) (z.highKey - z.lowKey + 1) * kw,
             (float) (z.highVel - z.lowVel + 1) * rh };
}

int ZoneMapView::zoneAt (juce::Point<float> pos) const
{
    if (selected >= 0 && selected < (int) zones.size() && zones[(size_t) selected].enabled
        && zoneRect (zones[(size_t) selected]).expanded (4.0f).contains (pos))
        return selected;

    for (int i = (int) zones.size() - 1; i >= 0; --i)
        if (zones[(size_t) i].enabled && zoneRect (zones[(size_t) i]).contains (pos))
            return i;
    return -1;
}

ZoneMapView::Mode ZoneMapView::modeAt (int i, juce::Point<float> pos) const
{
    if (i < 0) return None;
    auto r = zoneRect (zones[(size_t) i]);
    const float e = juce::jlimit (3.0f, 8.0f, juce::jmin (r.getWidth(), r.getHeight()) * 0.25f);
    if (std::abs (pos.x - r.getX()) <= e)      return Left;
    if (std::abs (pos.x - r.getRight()) <= e)  return Right;
    if (std::abs (pos.y - r.getY()) <= e)      return Top;
    if (std::abs (pos.y - r.getBottom()) <= e) return Bottom;
    return Move;
}

void ZoneMapView::paint (juce::Graphics& g)
{
    g.setColour (Pal::bg);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    auto p = plotArea();
    const float kw = p.getWidth() / 128.0f;
    const float rh = p.getHeight() / 127.0f;

    // crne dirke malo tamnije
    for (int n = 0; n < 128; ++n)
    {
        const int pc = n % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        const float x = p.getX() + (float) n * kw;
        if (black)
        {
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.fillRect (x, p.getY(), kw, p.getHeight());
        }
        if (held[(size_t) n])
        {
            g.setColour (Pal::part[0].withAlpha (0.35f));
            g.fillRect (x, p.getY(), kw, p.getHeight());
        }
    }

    // linije za C i oznake
    g.setFont (juce::FontOptions (11.0f));
    for (int n = 0; n < 128; n += 12)
    {
        const float x = p.getX() + (float) n * kw;
        g.setColour (Pal::panelHi.brighter (0.25f));
        g.drawVerticalLine (juce::roundToInt (x), p.getY(), p.getBottom());
        g.setColour (Pal::dim);
        g.drawText (juce::MidiMessage::getMidiNoteName (n, true, true, c3 ? 3 : 4),
                    juce::Rectangle<float> (x - 2.0f, p.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
    }
    for (int v : { 32, 64, 96, 127 })
    {
        const float y = p.getY() + (float) (127 - v) * rh;
        g.setColour (Pal::panelHi.brighter (0.15f));
        g.drawHorizontalLine (juce::roundToInt (y), p.getX(), p.getRight());
        g.setColour (Pal::dim);
        g.drawText (juce::String (v), juce::Rectangle<float> (p.getX() - 28.0f, y - 7.0f, 24.0f, 14.0f), juce::Justification::centredRight);
    }

    // zone (izabrana se crta poslednja)
    auto drawZone = [&] (int i, bool isSel)
    {
        const auto& z = zones[(size_t) i];
        auto r = zoneRect (z);
        auto c = colourFor (z.sample);
        g.setColour (c.withAlpha (isSel ? 0.55f : 0.28f));
        g.fillRect (r);
        g.setColour (isSel ? juce::Colours::white : c.withAlpha (0.8f));
        g.drawRect (r, isSel ? 2.0f : 1.0f);

        // root nota
        const float rx = p.getX() + ((float) z.rootKey + 0.5f) * kw;
        if (rx >= r.getX() - kw && rx <= r.getRight() + kw)
        {
            juce::Path tri;
            tri.addTriangle (rx - 4.0f, r.getBottom(), rx + 4.0f, r.getBottom(), rx, r.getBottom() - 7.0f);
            g.setColour (isSel ? juce::Colours::white : c);
            g.fillPath (tri);
        }
    };

    for (int i = 0; i < (int) zones.size(); ++i)
        if (zones[(size_t) i].enabled && i != selected)
            drawZone (i, false);
    if (selected >= 0 && selected < (int) zones.size() && zones[(size_t) selected].enabled)
        drawZone (selected, true);

    if (zones.empty())
    {
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (16.0f));
        g.drawText (T ("Nema zona"), p, juce::Justification::centred);
    }
}

void ZoneMapView::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.position;
    const int i = zoneAt (pos);
    if (i != selected && onSelect)
        onSelect (i);
    selected = i;

    mode = modeAt (i, pos);
    if (i >= 0)
        dragStartZone = zones[(size_t) i];
    dragStart = pos;
    dragged = false;
    repaint();
}

void ZoneMapView::mouseDrag (const juce::MouseEvent& e)
{
    if (mode == None || selected < 0 || selected >= (int) zones.size())
        return;

    auto p = plotArea();
    const int dk = juce::roundToInt ((e.position.x - dragStart.x) / (p.getWidth() / 128.0f));
    const int dv = juce::roundToInt (-(e.position.y - dragStart.y) / (p.getHeight() / 127.0f));
    const auto& s = dragStartZone;
    Zone z = s;

    switch (mode)
    {
        case Move:
        {
            const int kWidth = s.highKey - s.lowKey;
            const int vWidth = s.highVel - s.lowVel;
            z.lowKey = juce::jlimit (0, 127 - kWidth, s.lowKey + dk);
            z.highKey = z.lowKey + kWidth;
            z.rootKey = juce::jlimit (0, 127, s.rootKey + (z.lowKey - s.lowKey));
            z.lowVel = juce::jlimit (1, 127 - vWidth, s.lowVel + dv);
            z.highVel = z.lowVel + vWidth;
            break;
        }
        case Left:   z.lowKey = juce::jlimit (0, s.highKey, s.lowKey + dk); break;
        case Right:  z.highKey = juce::jlimit (s.lowKey, 127, s.highKey + dk); break;
        case Top:    z.highVel = juce::jlimit (s.lowVel, 127, s.highVel + dv); break;
        case Bottom: z.lowVel = juce::jlimit (1, s.highVel, s.lowVel + dv); break;
        case None:   break;
    }

    auto& cur = zones[(size_t) selected];
    if (z.lowKey != cur.lowKey || z.highKey != cur.highKey || z.lowVel != cur.lowVel
        || z.highVel != cur.highVel || z.rootKey != cur.rootKey)
    {
        if (! dragged && onChange)
            onChange (zones, false);   // stanje pre prvog pomeranja (za Ponisti)
        dragged = true;
        cur = z;
        repaint();
        if (onChange)
            onChange (zones, true);
    }
}

void ZoneMapView::mouseUp (const juce::MouseEvent&)
{
    mode = None;
    dragged = false;
}

void ZoneMapView::mouseMove (const juce::MouseEvent& e)
{
    const int i = zoneAt (e.position);
    switch (i == selected ? modeAt (i, e.position) : (i >= 0 ? Move : None))
    {
        case Left: case Right:  setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Top:  case Bottom: setMouseCursor (juce::MouseCursor::UpDownResizeCursor); break;
        case Move:              setMouseCursor (i == selected ? juce::MouseCursor::DraggingHandCursor
                                                              : juce::MouseCursor::PointingHandCursor); break;
        case None:              setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

//==============================================================================
//  MappingEditor
//==============================================================================
enum Columns { colNum = 1, colSample, colRoot, colLow, colHigh, colVel, colTune, colGain, colLoop };

MappingEditor::MappingEditor (LiveSamplerProcessor& p)
    : proc (p), mapView (p.keyboardState)
{
    setWantsKeyboardFocus (true);

    title.setText ("MAPIRANJE", juce::dontSendNotification);
    title.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, Pal::master);
    addAndMakeVisible (title);

    for (int i = 0; i < LiveSamplerProcessor::numParts; ++i)
    {
        partButtons[i].setButtonText (LiveSamplerProcessor::partName (i));
        partButtons[i].setColour (juce::TextButton::buttonOnColourId, Pal::part[i]);
        partButtons[i].onClick = [this, i] { setPart (i); };
        addAndMakeVisible (partButtons[i]);
    }

    instLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (instLabel);
    stateLabel.setFont (juce::FontOptions (13.0f));
    stateLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (stateLabel);

    undoButton.setButtonText (T ("Poništi"));
    undoButton.setTooltip ("Ctrl+Z");
    undoButton.onClick = [this] { undo(); };
    saveButton.setButtonText (T ("Sačuvaj"));
    saveButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f7d4a));
    saveButton.onClick = [this]
    {
        if (instName.isEmpty()) return;
        if (proc.saveMap (instName))
        {
            dirtyNames.erase (instName);
            refresh();
        }
        else
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Live Sampler",
                T ("Ne mogu da sačuvam mapu (proveri da li je folder zaštićen od upisa)."));
        }
    };
    revertButton.setButtonText (T ("Odbaci izmene"));
    revertButton.setTooltip (T ("Vrati poslednju sačuvanu mapu"));
    revertButton.onClick = [this]
    {
        if (instName.isEmpty()) return;
        dirtyNames.erase (instName);
        undoStack.clear();
        proc.reloadInstrument (instName, false);
    };
    resetButton.setButtonText (T ("Automatsko"));
    resetButton.setTooltip (T ("Napravi mapu ponovo iz fajla (KMP/NKI/nazivi). Sačuvana mapa se briše."));
    resetButton.onClick = [this]
    {
        if (instName.isEmpty() || inst == nullptr) return;
        inst->getMapFile().deleteFile();
        dirtyNames.erase (instName);
        undoStack.clear();
        proc.reloadInstrument (instName, true);
    };
    closeButton.setButtonText (T ("Zatvori"));
    closeButton.onClick = [this] { if (onClose) onClose(); };

    for (auto* b : { &undoButton, &saveButton, &revertButton, &resetButton, &closeButton })
        addAndMakeVisible (b);

    // grafik
    mapView.onSelect = [this] (int i) { select (i); };
    mapView.onChange = [this] (const std::vector<Zone>& z, bool afterChange)
    {
        if (! afterChange) { undoStack.push_back (zones); return; }
        commit (z, false);
    };
    addAndMakeVisible (mapView);

    // tabela
    table.setModel (this);
    table.setColour (juce::ListBox::backgroundColourId, Pal::bg);
    table.setRowHeight (24);
    auto& h = table.getHeader();
    h.addColumn ("#", colNum, 36, 30, 50, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Sempl", colSample, 250, 100, 600, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Root", colRoot, 70, 50, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Od", colLow, 70, 50, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Do", colHigh, 70, 50, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Velocity", colVel, 80, 60, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn (T ("Štim"), colTune, 60, 50, 100, juce::TableHeaderComponent::notSortable);
    h.addColumn (T ("Jačina"), colGain, 70, 50, 100, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Loop", colLoop, 60, 40, 100, juce::TableHeaderComponent::notSortable);
    h.setColour (juce::TableHeaderComponent::backgroundColourId, Pal::panelHi);
    h.setColour (juce::TableHeaderComponent::textColourId, Pal::text);
    addAndMakeVisible (table);

    // inspektor
    sampleInfo.setFont (juce::FontOptions (13.0f));
    sampleInfo.setJustificationType (juce::Justification::topLeft);
    sampleInfo.setColour (juce::Label::textColourId, Pal::text);
    addAndMakeVisible (sampleInfo);

    const char* labels[] = { "Root", "Od tastera", "Do tastera", "Vel. od", "Vel. do", "Štim", "Jačina", "Pan" };
    for (int i = 0; i < 8; ++i)
    {
        sliderLabels[i].setText (T (labels[i]), juce::dontSendNotification);
        sliderLabels[i].setFont (juce::FontOptions (13.0f, juce::Font::bold));
        sliderLabels[i].setColour (juce::Label::textColourId, Pal::dim);
        addAndMakeVisible (sliderLabels[i]);
    }

    for (auto* s : { &rootSlider, &lowKeySlider, &highKeySlider })
        setupKeySlider (*s);
    setupValueSlider (lowVelSlider, 1, 127, 1, {});
    setupValueSlider (highVelSlider, 1, 127, 1, {});
    setupValueSlider (tuneSlider, -100, 100, 1, " c");
    setupValueSlider (gainSlider, -40, 24, 0.5, " dB");
    setupValueSlider (panSlider, -1, 1, 0.02, {});

    rootSlider.onValueChange    = [this] { editSelected ([this] (Zone& z) { z.rootKey = (int) rootSlider.getValue(); }); };
    lowKeySlider.onValueChange  = [this] { editSelected ([this] (Zone& z) { z.lowKey = (int) lowKeySlider.getValue(); z.highKey = juce::jmax (z.highKey, z.lowKey); }); };
    highKeySlider.onValueChange = [this] { editSelected ([this] (Zone& z) { z.highKey = (int) highKeySlider.getValue(); z.lowKey = juce::jmin (z.lowKey, z.highKey); }); };
    lowVelSlider.onValueChange  = [this] { editSelected ([this] (Zone& z) { z.lowVel = (int) lowVelSlider.getValue(); z.highVel = juce::jmax (z.highVel, z.lowVel); }); };
    highVelSlider.onValueChange = [this] { editSelected ([this] (Zone& z) { z.highVel = (int) highVelSlider.getValue(); z.lowVel = juce::jmin (z.lowVel, z.highVel); }); };
    tuneSlider.onValueChange    = [this] { editSelected ([this] (Zone& z) { z.tuneCents = (float) tuneSlider.getValue(); }); };
    gainSlider.onValueChange    = [this] { editSelected ([this] (Zone& z) { z.gainDb = (float) gainSlider.getValue(); }); };
    panSlider.onValueChange     = [this] { editSelected ([this] (Zone& z) { z.pan = (float) panSlider.getValue(); }); };

    enabledToggle.setButtonText (T ("Uključena"));
    loopToggle.setButtonText ("Loop");
    fixedToggle.setButtonText (T ("Ista visina"));
    fixedToggle.setTooltip (T ("Sempl svira istom visinom na svim tasterima (bubnjevi, efekti)"));
    enabledToggle.onClick = [this] { editSelected ([this] (Zone& z) { z.enabled = enabledToggle.getToggleState(); }, false); };
    loopToggle.onClick    = [this] { editSelected ([this] (Zone& z) { z.loop = loopToggle.getToggleState(); }, false); };
    fixedToggle.onClick   = [this] { editSelected ([this] (Zone& z) { z.fixedPitch = fixedToggle.getToggleState(); }, false); };
    for (auto* t : { &enabledToggle, &loopToggle, &fixedToggle })
        addAndMakeVisible (t);

    duplicateButton.setButtonText ("Dupliraj zonu");
    duplicateButton.onClick = [this]
    {
        if (selected < 0) return;
        auto z = zones;
        z.insert (z.begin() + selected + 1, z[(size_t) selected]);
        commit (z, true);
        select (selected + 1);
    };
    deleteButton.setButtonText (T ("Obriši zonu"));
    deleteButton.onClick = [this]
    {
        if (selected < 0) return;
        auto z = zones;
        z.erase (z.begin() + selected);
        commit (z, true);
        select (juce::jmin (selected, (int) zones.size() - 1));
    };
    octDownButton.setButtonText (T ("Sve − oktava"));
    octDownButton.setTooltip (T ("Pomeri sve zone i root note oktavu niže"));
    octUpButton.setButtonText (T ("Sve + oktava"));
    octUpButton.setTooltip (T ("Pomeri sve zone i root note oktavu više"));
    auto shiftAll = [this] (int semis)
    {
        auto z = zones;
        for (auto& x : z)
        {
            // ivice klavijature ostaju na ivicama (zona 0-40 postaje 0-52)
            x.rootKey = juce::jlimit (0, 127, x.rootKey + semis);
            if (x.lowKey > 0)    x.lowKey = juce::jlimit (0, 127, x.lowKey + semis);
            if (x.highKey < 127) x.highKey = juce::jlimit (0, 127, x.highKey + semis);
            x.highKey = juce::jmax (x.highKey, x.lowKey);
        }
        commit (z, true);
    };
    octDownButton.onClick = [shiftAll] { shiftAll (-12); };
    octUpButton.onClick = [shiftAll] { shiftAll (12); };

    rangesButton.setButtonText (T ("Rasponi po root"));
    rangesButton.setTooltip (T ("Ponovo podeli tastere između zona prema root notama (za svaki velocity sloj)"));
    rangesButton.onClick = [this]
    {
        auto z = zones;
        std::map<std::pair<int, int>, std::vector<size_t>> layers;
        for (size_t i = 0; i < z.size(); ++i)
            if (z[i].enabled)
                layers[{ z[i].lowVel, z[i].highVel }].push_back (i);

        for (auto& kv : layers)
        {
            std::vector<int> roots;
            for (auto i : kv.second)
                if (std::find (roots.begin(), roots.end(), z[i].rootKey) == roots.end())
                    roots.push_back (z[i].rootKey);
            std::sort (roots.begin(), roots.end());

            for (auto i : kv.second)
            {
                auto r = (size_t) (std::find (roots.begin(), roots.end(), z[i].rootKey) - roots.begin());
                z[i].lowKey = r == 0 ? 0 : (roots[r - 1] + roots[r]) / 2 + 1;
                z[i].highKey = r == roots.size() - 1 ? 127 : (roots[r] + roots[r + 1]) / 2;
            }
        }
        commit (z, true);
    };

    for (auto* b : { &duplicateButton, &deleteButton, &octDownButton, &octUpButton, &rangesButton })
        addAndMakeVisible (b);

    hint.setText (T ("Klikni zonu da je izabereš, prevuci je ili joj vuci ivice. Sviraj da čuješ promene odmah."),
                  juce::dontSendNotification);
    hint.setFont (juce::FontOptions (13.0f));
    hint.setMinimumHorizontalScale (1.0f);
    hint.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (hint);

    setPart (0);
}

juce::String MappingEditor::noteName (int n) const
{
    return juce::MidiMessage::getMidiNoteName (n, true, true, proc.getC3Is60() ? 3 : 4);
}

void MappingEditor::setupKeySlider (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::IncDecButtons);
    s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 26);
    s.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    s.setRange (0, 127, 1);
    s.textFromValueFunction = [this] (double v) { return noteName ((int) v) + " (" + juce::String ((int) v) + ")"; };
    s.valueFromTextFunction = [this] (const juce::String& t)
    {
        int note, layer;
        auto txt = t.upToFirstOccurrenceOf ("(", false, false).trim();
        if (txt.containsOnly ("0123456789"))
            return (double) txt.getIntValue();
        if (SampleLibrary::parseFileName (txt, proc.getC3Is60(), note, layer))
            return (double) note;
        return (double) t.getIntValue();
    };
    s.setColour (juce::Slider::textBoxTextColourId, Pal::text);
    s.updateText();
    addAndMakeVisible (s);
}

void MappingEditor::setupValueSlider (juce::Slider& s, double lo, double hi, double step, const juce::String& suffix)
{
    s.setSliderStyle (juce::Slider::IncDecButtons);
    s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 26);
    s.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    s.setRange (lo, hi, step);
    s.setTextValueSuffix (suffix);
    s.setColour (juce::Slider::textBoxTextColourId, Pal::text);
    addAndMakeVisible (s);
}

//==============================================================================
void MappingEditor::setPart (int p)
{
    part = juce::jlimit (0, LiveSamplerProcessor::numParts - 1, p);
    for (int i = 0; i < LiveSamplerProcessor::numParts; ++i)
        partButtons[i].setToggleState (i == part, juce::dontSendNotification);
    refresh();
}

void MappingEditor::refresh()
{
    const auto name = proc.getInstrumentName (part);
    auto cur = proc.getLoadedInstrument (name);

    if (name != instName)
    {
        instName = name;
        undoStack.clear();
        selected = -1;
        inst = nullptr;
    }

    if (cur != inst)
    {
        inst = cur;
        zones = cur != nullptr ? cur->zones : std::vector<Zone>();
        if (selected >= (int) zones.size())
            selected = (int) zones.size() - 1;
        table.updateContent();
        if (selected < 0 && ! zones.empty())
            select (0);
        else
            select (selected);
    }

    updateHeader();
}

void MappingEditor::updateHeader()
{
    if (instName.isEmpty())
        instLabel.setText (T ("(nema instrumenta na ovom delu)"), juce::dontSendNotification);
    else
        instLabel.setText (instName + (inst != nullptr ? "   [" + sourceTypeName (inst->type) + "]" : juce::String()),
                           juce::dontSendNotification);

    juce::String state;
    juce::Colour col = Pal::dim;
    if (instName.isNotEmpty() && inst == nullptr)
        state = T ("Učitavam...");
    else if (dirtyNames.count (instName) > 0)
    {
        state = T ("● Nesačuvane izmene");
        col = juce::Colour (0xffffb340);
    }
    else if (inst != nullptr)
        state = inst->fromMapFile ? T ("Sačuvana mapa") : T ("Automatska mapa");

    if (inst != nullptr && ! inst->warnings.isEmpty())
        state = state + T ("   ⚠ ") + inst->warnings[0];

    stateLabel.setText (state, juce::dontSendNotification);
    stateLabel.setColour (juce::Label::textColourId, col);
    stateLabel.setTooltip (inst != nullptr ? inst->warnings.joinIntoString ("\n") : juce::String());

    const bool has = inst != nullptr;
    undoButton.setEnabled (! undoStack.empty());
    saveButton.setEnabled (has);
    revertButton.setEnabled (has && dirtyNames.count (instName) > 0);
    resetButton.setEnabled (has);
}

void MappingEditor::commit (std::vector<Zone> newZones, bool pushUndo, bool coalesce)
{
    if (instName.isEmpty() || inst == nullptr)
        return;

    if (pushUndo)
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (! coalesce || now - lastUndoPush > 700)
            undoStack.push_back (zones);
        lastUndoPush = now;
        if (undoStack.size() > 200)
            undoStack.erase (undoStack.begin());
    }

    zones = std::move (newZones);
    dirtyNames.insert (instName);
    proc.applyZones (instName, zones);
    inst = proc.getLoadedInstrument (instName);

    table.updateContent();
    table.repaint();
    if (selected >= (int) zones.size())
        selected = (int) zones.size() - 1;
    mapView.setZones (zones, selected, proc.getC3Is60());
    updateInspector();
    updateHeader();
}

void MappingEditor::undo()
{
    if (undoStack.empty())
        return;
    auto z = undoStack.back();
    undoStack.pop_back();
    commit (z, false);
}

void MappingEditor::editSelected (std::function<void (Zone&)> fn, bool coalesce)
{
    if (updatingUI || selected < 0 || selected >= (int) zones.size())
        return;
    auto z = zones;
    fn (z[(size_t) selected]);
    commit (z, true, coalesce);
}

void MappingEditor::select (int index)
{
    selected = (index >= 0 && index < (int) zones.size()) ? index : -1;
    {
        const juce::ScopedValueSetter<bool> svs (updatingUI, true);
        if (selected >= 0) table.selectRow (selected, false, true);
        else               table.deselectAllRows();
    }
    if (selected >= 0)
        table.scrollToEnsureRowIsOnscreen (selected);
    mapView.setZones (zones, selected, proc.getC3Is60());
    updateInspector();
}

void MappingEditor::updateInspector()
{
    const juce::ScopedValueSetter<bool> svs (updatingUI, true);
    const bool has = selected >= 0 && selected < (int) zones.size() && inst != nullptr;

    for (auto* c : std::initializer_list<juce::Component*> { &rootSlider, &lowKeySlider, &highKeySlider, &lowVelSlider, &highVelSlider,
                                                              &tuneSlider, &gainSlider, &panSlider, &enabledToggle, &loopToggle,
                                                              &fixedToggle, &duplicateButton, &deleteButton })
        c->setEnabled (has);

    const bool anyZones = ! zones.empty();
    octDownButton.setEnabled (anyZones);
    octUpButton.setEnabled (anyZones);
    rangesButton.setEnabled (anyZones);

    if (! has)
    {
        sampleInfo.setText (T ("Izaberi zonu na grafiku ili u tabeli."), juce::dontSendNotification);
        return;
    }

    const auto& z = zones[(size_t) selected];
    rootSlider.setValue (z.rootKey, juce::dontSendNotification);
    lowKeySlider.setValue (z.lowKey, juce::dontSendNotification);
    highKeySlider.setValue (z.highKey, juce::dontSendNotification);
    lowVelSlider.setValue (z.lowVel, juce::dontSendNotification);
    highVelSlider.setValue (z.highVel, juce::dontSendNotification);
    tuneSlider.setValue (z.tuneCents, juce::dontSendNotification);
    gainSlider.setValue (z.gainDb, juce::dontSendNotification);
    panSlider.setValue (z.pan, juce::dontSendNotification);
    enabledToggle.setToggleState (z.enabled, juce::dontSendNotification);
    loopToggle.setToggleState (z.loop, juce::dontSendNotification);
    fixedToggle.setToggleState (z.fixedPitch, juce::dontSendNotification);

    if (z.sample >= 0 && z.sample < (int) inst->samples.size())
    {
        const auto& s = *inst->samples[(size_t) z.sample];
        loopToggle.setEnabled (s.hasLoop || z.hasOwnLoop());
        juce::String info;
        info << s.displayName << "\n"
             << juce::String (s.lengthSeconds(), 2) << " s  |  " << juce::String (s.sampleRate / 1000.0, 1) << " kHz  |  "
             << (z.hasOwnLoop() ? T ("loop iz NKI zone") : (s.hasLoop ? T ("ima loop") : T ("bez loop-a"))) << "\n"
             << s.id;
        sampleInfo.setText (info, juce::dontSendNotification);
    }
}

//==============================================================================
int MappingEditor::getNumRows()
{
    return (int) zones.size();
}

void MappingEditor::paintRowBackground (juce::Graphics& g, int row, int w, int h, bool isSelected)
{
    if (isSelected)
        g.fillAll (Pal::master.withAlpha (0.3f));
    else if (row % 2 == 1)
        g.fillAll (Pal::panel.withAlpha (0.5f));

    if (row >= 0 && row < (int) zones.size())
    {
        g.setColour (ZoneMapView::colourFor (zones[(size_t) row].sample));
        g.fillRect (0, 3, 4, h - 6);
    }
    juce::ignoreUnused (w);
}

void MappingEditor::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    if (row < 0 || row >= (int) zones.size() || inst == nullptr)
        return;

    const auto& z = zones[(size_t) row];
    g.setColour (z.enabled ? Pal::text : Pal::dim.withAlpha (0.6f));
    g.setFont (juce::FontOptions (13.5f));

    juce::String t;
    switch (col)
    {
        case colNum:    t = juce::String (row + 1); break;
        case colSample: t = (z.sample >= 0 && z.sample < (int) inst->samples.size() ? inst->samples[(size_t) z.sample]->displayName : "?")
                            + (z.enabled ? juce::String() : T ("  (isključena)")); break;
        case colRoot:   t = noteName (z.rootKey) + (z.fixedPitch ? " *" : ""); break;
        case colLow:    t = noteName (z.lowKey); break;
        case colHigh:   t = noteName (z.highKey); break;
        case colVel:    t = juce::String (z.lowVel) + "-" + juce::String (z.highVel); break;
        case colTune:   t = juce::String (juce::roundToInt (z.tuneCents)); break;
        case colGain:   t = juce::String (z.gainDb, 1); break;
        case colLoop:
        {
            const bool has = z.hasOwnLoop() || (z.sample >= 0 && z.sample < (int) inst->samples.size() && inst->samples[(size_t) z.sample]->hasLoop);
            t = ! has ? "-" : (z.loop ? T ("da") : T ("isklj."));
            break;
        }
        default: break;
    }
    g.drawText (t, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

void MappingEditor::selectedRowsChanged (int lastRowSelected)
{
    if (updatingUI)
        return;
    selected = lastRowSelected;
    mapView.setZones (zones, selected, proc.getC3Is60());
    updateInspector();
}

bool MappingEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        undo();
        return true;
    }
    if (k == juce::KeyPress::deleteKey && selected >= 0)
    {
        deleteButton.triggerClick();
        return true;
    }
    return false;
}

//==============================================================================
void MappingEditor::paint (juce::Graphics& g)
{
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    g.setColour (Pal::master);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 10.0f, 1.5f);
}

void MappingEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    auto header = r.removeFromTop (34);
    title.setBounds (header.removeFromLeft (130));
    for (auto& b : partButtons)
    {
        b.setBounds (header.removeFromLeft (120).reduced (2, 0));
    }
    header.removeFromLeft (12);
    closeButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (6);
    resetButton.setBounds (header.removeFromRight (110));
    header.removeFromRight (6);
    revertButton.setBounds (header.removeFromRight (130));
    header.removeFromRight (6);
    saveButton.setBounds (header.removeFromRight (100));
    header.removeFromRight (6);
    undoButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (10);
    instLabel.setBounds (header);

    r.removeFromTop (4);
    auto stateRow = r.removeFromTop (20);
    stateLabel.setBounds (stateRow.removeFromRight (stateRow.getWidth() * 2 / 5));
    hint.setBounds (stateRow);

    r.removeFromTop (4);
    mapView.setBounds (r.removeFromTop (250));
    r.removeFromTop (10);

    auto inspector = r.removeFromRight (420);
    r.removeFromRight (10);
    table.setBounds (r);

    sampleInfo.setBounds (inspector.removeFromTop (54));
    inspector.removeFromTop (4);

    juce::Slider* sliders[] = { &rootSlider, &lowKeySlider, &highKeySlider, &tuneSlider,
                                &lowVelSlider, &highVelSlider, &gainSlider, &panSlider };
    const int labelIdx[] = { 0, 1, 2, 5, 3, 4, 6, 7 };
    const int colW = inspector.getWidth() / 2;
    for (int row = 0; row < 4; ++row)
    {
        auto line = inspector.removeFromTop (32);
        for (int c = 0; c < 2; ++c)
        {
            const int i = c * 4 + row;
            auto cell = line.removeFromLeft (colW).reduced (2, 2);
            sliderLabels[labelIdx[i]].setBounds (cell.removeFromLeft (72));
            sliders[i]->setBounds (cell);
        }
    }

    inspector.removeFromTop (4);
    auto toggles = inspector.removeFromTop (26);
    enabledToggle.setBounds (toggles.removeFromLeft (130));
    loopToggle.setBounds (toggles.removeFromLeft (100));
    fixedToggle.setBounds (toggles);

    inspector.removeFromTop (6);
    auto b1 = inspector.removeFromTop (30);
    duplicateButton.setBounds (b1.removeFromLeft (b1.getWidth() / 2).reduced (2, 0));
    deleteButton.setBounds (b1.reduced (2, 0));
    inspector.removeFromTop (6);
    auto b2 = inspector.removeFromTop (30);
    const int bw = b2.getWidth() / 3;
    octDownButton.setBounds (b2.removeFromLeft (bw).reduced (2, 0));
    octUpButton.setBounds (b2.removeFromLeft (bw).reduced (2, 0));
    rangesButton.setBounds (b2.reduced (2, 0));
}

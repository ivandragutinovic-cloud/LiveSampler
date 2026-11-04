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

void ZoneMapView::setZones (const std::vector<Zone>& z, int selectedIndex, bool c3Is60, const std::set<int>& multiSel)
{
    if (mode == None)
        zones = z;
    selected = selectedIndex;
    multi = multiSel;
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
        if (zones[(size_t) i].enabled && i != selected && multi.count (i) == 0)
            drawZone (i, false);
    for (int i : multi)
        if (i != selected && i >= 0 && i < (int) zones.size() && zones[(size_t) i].enabled)
            drawZone (i, true);
    if (selected >= 0 && selected < (int) zones.size() && zones[(size_t) selected].enabled)
        drawZone (selected, true);
    if (boxing)
    {
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRect (box);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawRect (box, 1.0f);
    }

    if (zones.empty())
    {
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (16.0f));
        g.drawText (T ("No zones"), p, juce::Justification::centred);
    }
}

void ZoneMapView::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.position;
    const int i = zoneAt (pos);
    const bool ctrl = e.mods.isCommandDown() || e.mods.isCtrlDown();
    const bool shift = e.mods.isShiftDown();
    mode = None;
    dragged = false;
    dragStart = pos;
    boxing = false;

    if (i < 0)
    {
        // prazno mesto: okvir za vise zona (sa Ctrl / Shift se dodaje izabranima)
        if (! ctrl && ! shift && onSelect) onSelect (-1);
        boxing = true;
        box = { pos, pos };
        repaint();
        return;
    }
    if (ctrl) { if (onToggle) onToggle (i); repaint(); return; }
    if (shift) { if (onRange) onRange (i); repaint(); return; }

    // klik na zonu koja je vec u grupi: grupa ostaje (povlace se sve zajedno)
    if (multi.count (i) == 0 || multi.size() <= 1)
    {
        if (i != selected && onSelect) onSelect (i);
        multi = { i };
    }
    selected = i;
    mode = modeAt (i, pos);
    dragStartZone = zones[(size_t) i];
    dragStartAll = zones;
    repaint();
}

void ZoneMapView::mouseDrag (const juce::MouseEvent& e)
{
    if (boxing)
    {
        box = juce::Rectangle<float> (dragStart, e.position);
        repaint();
        return;
    }
    if (mode == None || selected < 0 || selected >= (int) zones.size())
        return;

    auto p = plotArea();
    const int dk = juce::roundToInt ((e.position.x - dragStart.x) / (p.getWidth() / 128.0f));
    const int dv = juce::roundToInt (-(e.position.y - dragStart.y) / (p.getHeight() / 127.0f));

    auto apply = [this, dk, dv] (const Zone& s) -> Zone
    {
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
        return z;
    };

    auto next = zones;
    bool changed = false;
    // grupa: isti pomak za sve izabrane (jedna zona: samo ona)
    std::set<int> group = multi.size() > 1 && multi.count (selected) ? multi : std::set<int> { selected };
    for (int k : group)
    {
        if (k < 0 || k >= (int) next.size() || k >= (int) dragStartAll.size()) continue;
        const auto z = apply (dragStartAll[(size_t) k]);
        auto& cur = next[(size_t) k];
        if (z.lowKey != cur.lowKey || z.highKey != cur.highKey || z.lowVel != cur.lowVel || z.highVel != cur.highVel || z.rootKey != cur.rootKey)
        {
            cur = z;
            changed = true;
        }
    }
    if (changed)
    {
        if (! dragged && onChange)
            onChange (zones, false);   // stanje pre prvog pomeranja (za Ponisti)
        dragged = true;
        zones = next;
        repaint();
        if (onChange)
            onChange (zones, true);
    }
}

void ZoneMapView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int i = zoneAt (e.position);
    if (i >= 0 && onPreview) onPreview (i);
}

void ZoneMapView::mouseUp (const juce::MouseEvent& e)
{
    if (boxing)
    {
        boxing = false;
        std::vector<int> hits;
        if (box.getWidth() > 3.0f || box.getHeight() > 3.0f)
            for (int i = 0; i < (int) zones.size(); ++i)
                if (zones[(size_t) i].enabled && zoneRect (zones[(size_t) i]).intersects (box))
                    hits.push_back (i);
        if (! hits.empty() && onBox)
            onBox (hits, e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isShiftDown());
        repaint();
    }
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

//==============================================================================
//  WaveEditor
//==============================================================================
WaveEditor::WaveEditor()
{
    sampleMode.setButtonText ("SAMPLE EDIT");
    loopMode.setButtonText ("LOOP EDIT");
    for (auto* b : { &sampleMode, &loopMode })
    {
        b->setColour (juce::TextButton::buttonOnColourId, Pal::master);
        b->getProperties().set ("fontSize", 12.0f);
        addAndMakeVisible (b);
    }
    sampleMode.onClick = [this] { loopView = false; sampleMode.setToggleState (true, juce::dontSendNotification); loopMode.setToggleState (false, juce::dontSendNotification); repaint(); };
    loopMode.onClick   = [this] { loopView = true;  sampleMode.setToggleState (false, juce::dontSendNotification); loopMode.setToggleState (true, juce::dontSendNotification); repaint(); };
    sampleMode.setToggleState (true, juce::dontSendNotification);

    fitButton.setButtonText (T ("Full"));
    fitButton.setTooltip (T ("Show the whole sample (mouse wheel zooms, dragging an empty area moves the view)"));
    fitButton.getProperties().set ("fontSize", 12.0f);
    fitButton.onClick = [this] { zoomToFit(); repaint(); };
    addAndMakeVisible (fitButton);

    loopToggle.setButtonText ("Loop");
    loopToggle.onClick = [this]
    {
        auto np = pts;
        np.loop = loopToggle.getToggleState();
        if (np.loop && np.loopStart >= np.end - 16)
            np.loopStart = np.start;
        apply (np, true);
    };
    addAndMakeVisible (loopToggle);

    zeroToggle.setButtonText (T ("Zero-cross"));
    zeroToggle.setTooltip (T ("Points snap to the nearest zero crossing (fewer clicks)"));
    zeroToggle.setToggleState (true, juce::dontSendNotification);
    addAndMakeVisible (zeroToggle);

    autoButton.setButtonText ("AUTO LOOP");
    autoButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f7d4a));
    autoButton.setTooltip (T ("Finds a loop in the sustained part and sets the crossfade"));
    autoButton.onClick = [this] { runAutoLoop(); };
    autoAllButton.setButtonText (T ("AUTO LOOP ALL ZONES"));
    autoAllButton.setTooltip (T ("Auto loop for all zones of this instrument (sounds that do not sustain stay without a loop)"));
    autoAllButton.onClick = [this] { if (onAutoAll) onAutoAll(); };
    autoStartButton.setButtonText (T ("AUTO START"));
    autoStartButton.setTooltip (T ("Start at the note onset (cuts silence and noise before the note)"));
    autoStartButton.onClick = [this]
    {
        if (sample == nullptr) return;
        auto np = pts;
        np.start = AutoLoop::findOnset (sample->audio(), sample->length(), sample->sampleRate);
        if (np.loop && np.start >= np.loopStart) np.start = 0;
        apply (np, true);
    };
    autoStartAllButton.setButtonText (T ("AUTO START ALL ZONES"));
    autoStartAllButton.setTooltip (T ("For all zones: start at the note onset (e.g. soft velocities that come late)"));
    autoStartAllButton.onClick = [this] { if (onAutoStartAll) onAutoStartAll(); };
    vPlus.setButtonText (T ("Height +"));
    vMinus.setButtonText (T ("Height -"));
    vFit.setButtonText (T ("Height auto"));
    vPlus.setTooltip (T ("Vertical wave zoom (or Ctrl + wheel)"));
    vPlus.onClick = [this] { vZoom = juce::jlimit (1.0f, 1000.0f, vZoom * 2.0f); repaint(); };
    vMinus.onClick = [this] { vZoom = juce::jlimit (1.0f, 1000.0f, vZoom * 0.5f); repaint(); };
    vFit.onClick = [this] { fitHeight(); repaint(); };
    for (auto* b : { &autoButton, &autoAllButton, &autoStartButton, &autoStartAllButton, &vPlus, &vMinus, &vFit }) { b->getProperties().set ("fontSize", 12.0f); addAndMakeVisible (b); }

    auto setupMs = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, double def)
    {
        s.setSliderStyle (juce::Slider::IncDecButtons);
        s.setRange (lo, hi, 1);
        s.setValue (def, juce::dontSendNotification);
        s.setTextValueSuffix (" ms");
        s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 22);
        l.setText (name, juce::dontSendNotification);
        l.setFont (juce::FontOptions (12.0f));
        l.setColour (juce::Label::textColourId, Pal::dim);
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (s);
        addAndMakeVisible (l);
    };
    setupMs (xfadeMs, xfadeLabel, "Crossfade", 0, 1000, 80);
    setupMs (minLoopMs, minLabel, "Min loop", 30, 5000, 300);
    setupMs (maxLoopMs, maxLabel, "Max loop", 100, 20000, 3000);
    xfadeMs.onValueChange = [this]
    {
        if (updating || sample == nullptr) return;
        auto np = pts;
        np.xfade = (int) (xfadeMs.getValue() * 0.001 * sample->sampleRate);
        apply (np, true);
    };
    autoInfo.setFont (juce::FontOptions (12.0f));
    autoInfo.setColour (juce::Label::textColourId, Pal::lcdText);
    addAndMakeVisible (autoInfo);

    const char* names[] = { "Start", "Loop start", "End" };
    for (int i = 0; i < 3; ++i)
    {
        labels[i].setText (T (names[i]), juce::dontSendNotification);
        labels[i].setFont (juce::FontOptions (12.0f));
        labels[i].setColour (juce::Label::textColourId, Pal::dim);
        labels[i].setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (labels[i]);
        boxes[i].setSliderStyle (juce::Slider::IncDecButtons);
        boxes[i].setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 22);
        boxes[i].setRange (0, 1, 1);
        boxes[i].onValueChange = [this, i]
        {
            if (updating) return;
            auto np = pts;
            const int v = (int) boxes[i].getValue();
            (i == 0 ? np.start : i == 1 ? np.loopStart : np.end) = v;
            apply (np, true);
        };
        addAndMakeVisible (boxes[i]);
    }
    clear();
}

AutoLoop::Settings WaveEditor::autoSettings() const
{
    AutoLoop::Settings s;
    s.crossfadeSec = xfadeMs.getValue() * 0.001;
    s.minLoopSec = minLoopMs.getValue() * 0.001;
    s.maxLoopSec = juce::jmax (s.minLoopSec + 0.05, maxLoopMs.getValue() * 0.001);
    return s;
}

void WaveEditor::runAutoLoop()
{
    if (sample == nullptr) return;
    const auto r = AutoLoop::find (sample->audio(), pts.end > 0 ? juce::jmax (pts.end, sample->length()) : sample->length(),
                                   sample->sampleRate, autoSettings());
    if (! r.found)
    {
        autoInfo.setText (T ("Loop not found: ") + r.reason, juce::dontSendNotification);
        return;
    }
    auto np = pts;
    np.loop = true;
    np.loopStart = r.start;
    np.end = r.end;
    np.xfade = r.crossfade;
    np.start = juce::jmin (np.start, r.start);
    autoInfo.setText (T ("Loop found, match ") + juce::String (juce::roundToInt (r.quality * 100)) + " %", juce::dontSendNotification);
    loopView = true;
    sampleMode.setToggleState (false, juce::dontSendNotification);
    loopMode.setToggleState (true, juce::dontSendNotification);
    apply (np, true);
}

void WaveEditor::clear()
{
    sample = nullptr;
    pts = {};
    for (auto& b : boxes) b.setEnabled (false);
    loopToggle.setEnabled (false);
    repaint();
}

void WaveEditor::setSample (SamplePtr s, const Points& p)
{
    const bool newSample = s != sample;
    sample = s;
    pts = p;
    for (auto& b : boxes) b.setEnabled (sample != nullptr);
    loopToggle.setEnabled (sample != nullptr);
    if (newSample) zoomToFit();
    updateBoxes();
    repaint();
}

void WaveEditor::updateBoxes()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    const int len = sample != nullptr ? sample->length() : 1;
    for (auto& b : boxes) b.setRange (0, juce::jmax (1, len), 1);
    boxes[0].setValue (pts.start, juce::dontSendNotification);
    boxes[1].setValue (pts.loopStart, juce::dontSendNotification);
    boxes[2].setValue (pts.end, juce::dontSendNotification);
    boxes[1].setEnabled (sample != nullptr && pts.loop);
    xfadeMs.setValue (sample != nullptr ? pts.xfade * 1000.0 / sample->sampleRate : 0.0, juce::dontSendNotification);
    xfadeMs.setEnabled (sample != nullptr && pts.loop);
    labels[1].setAlpha (pts.loop ? 1.0f : 0.4f);
    loopToggle.setToggleState (pts.loop, juce::dontSendNotification);
}

void WaveEditor::zoomToFit()
{
    viewStart = 0.0;
    viewLen = sample != nullptr ? juce::jmax (16.0, (double) sample->length()) : 1.0;
}

int WaveEditor::snap (int pos) const
{
    if (sample == nullptr || ! zeroToggle.getToggleState()) return pos;
    const auto* d = sample->audio().getReadPointer (0);
    const int len = sample->length();
    for (int r = 0; r < 2000; ++r)
        for (int sgn : { 1, -1 })
        {
            const int i = pos + sgn * r;
            if (i > 0 && i < len && ((d[i - 1] <= 0.0f && d[i] >= 0.0f) || (d[i - 1] >= 0.0f && d[i] <= 0.0f)))
                return i;
        }
    return pos;
}

void WaveEditor::apply (Points np, bool finished)
{
    if (sample == nullptr) return;
    const int len = sample->length();
    np.end = juce::jlimit (32, len, np.end);
    np.start = juce::jlimit (0, np.end - 16, np.start);
    np.loopStart = juce::jlimit (np.start, np.end - 16, np.loopStart);
    np.xfade = juce::jlimit (0, juce::jmin (np.loopStart, (np.end - np.loopStart) / 2), np.xfade);
    pts = np;
    updateBoxes();
    repaint();
    if (onChange) onChange (pts, finished);
}

juce::Rectangle<int> WaveEditor::waveArea() const
{
    return getLocalBounds().withTrimmedTop (62).reduced (2);
}

double WaveEditor::xToSample (float x) const
{
    auto a = waveArea().toFloat();
    if (loopView)
    {
        // levo: kraj (pre End), desno: posle Loop start
        const float mid = a.getCentreX();
        const double perPx = (double) seamHalf / (a.getWidth() * 0.5);
        return x < mid ? pts.end - (mid - x) * perPx : pts.loopStart + (x - mid) * perPx;
    }
    return viewStart + (x - a.getX()) / a.getWidth() * viewLen;
}

float WaveEditor::sampleToX (double s) const
{
    auto a = waveArea().toFloat();
    return a.getX() + (float) ((s - viewStart) / viewLen) * a.getWidth();
}

WaveEditor::Marker WaveEditor::markerAt (float x) const
{
    if (sample == nullptr) return None;
    if (loopView)
    {
        // u LOOP prikazu se vuce sredina: levo = End, desno = Loop start
        auto a = waveArea().toFloat();
        if (std::abs (x - a.getCentreX()) < 10.0f) return End;
        return None;
    }
    Marker best = None;
    float bestD = 8.0f;
    const int vals[] = { pts.start, pts.loopStart, pts.end };
    for (int m = 0; m < 3; ++m)
    {
        if (m == LoopStart && ! pts.loop) continue;
        const float d = std::abs (sampleToX (vals[m]) - x);
        if (d < bestD) { bestD = d; best = (Marker) m; }
    }
    return best;
}

void WaveEditor::paint (juce::Graphics& g)
{
    auto a = waveArea();
    g.setColour (Pal::bg);
    g.fillRoundedRectangle (a.toFloat(), 4.0f);
    if (sample == nullptr)
    {
        g.setColour (Pal::dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (T ("Select a zone to see its sample."), a, juce::Justification::centred);
        return;
    }

    const auto* d = sample->audio().getReadPointer (0);
    const int len = sample->length();
    const float cy = (float) a.getCentreY(), hh = a.getHeight() * 0.46f * vZoom;

    // oblik talasa (min/max po pikselu)
    auto drawWave = [&] (juce::Colour c)
    {
        g.setColour (c);
        for (int px = a.getX(); px < a.getRight(); ++px)
        {
            const double s0 = xToSample ((float) px), s1 = xToSample ((float) px + 1.0f);
            int i0 = (int) std::floor (juce::jmin (s0, s1)), i1 = (int) std::ceil (juce::jmax (s0, s1));
            i0 = juce::jlimit (0, len - 1, i0);
            i1 = juce::jlimit (i0 + 1, len, i1);
            if (s1 < 0 || s0 >= len) continue;
            float mn = 1.0f, mx = -1.0f;
            const int step = juce::jmax (1, (i1 - i0) / 64);
            for (int i = i0; i < i1; i += step) { mn = juce::jmin (mn, d[i]); mx = juce::jmax (mx, d[i]); }
            const float top = juce::jlimit ((float) a.getY(), (float) a.getBottom(), cy - mx * hh);
            const float bot = juce::jlimit ((float) a.getY(), (float) a.getBottom(), cy - mn * hh + 1.0f);
            g.drawVerticalLine (px, top, juce::jmax (top + 1.0f, bot));
        }
    };

    if (loopView)
    {
        g.setColour (Pal::panelHi);
        g.drawHorizontalLine ((int) cy, (float) a.getX(), (float) a.getRight());
        drawWave (pts.loop ? juce::Colour (0xff6fd3ff) : Pal::dim);
        const float mid = (float) a.getCentreX();
        g.setColour (juce::Colour (0xffffd23d));
        g.drawVerticalLine ((int) mid, (float) a.getY(), (float) a.getBottom());
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (T ("← end (End)"), (int) mid - 110, a.getY() + 4, 104, 16, juce::Justification::centredRight);
        g.drawText (T ("Loop start →"), (int) mid + 6, a.getY() + 4, 104, 16, juce::Justification::centredLeft);
        g.setColour (Pal::dim);
        g.drawText (pts.loop ? T ("The join should be smooth. Drag the middle to move End, the wheel zooms.")
                             : T ("Loop is off."),
                    a.reduced (8, 4), juce::Justification::bottomLeft);
        return;
    }

    // osenci deo koji se ne svira
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    const float xs = sampleToX (pts.start), xe = sampleToX (pts.end);
    g.fillRect (juce::Rectangle<float> ((float) a.getX(), (float) a.getY(), juce::jmax (0.0f, xs - a.getX()), (float) a.getHeight()));
    g.fillRect (juce::Rectangle<float> (xe, (float) a.getY(), juce::jmax (0.0f, a.getRight() - xe), (float) a.getHeight()));
    if (pts.loop)
    {
        g.setColour (juce::Colour (0xffffd23d).withAlpha (0.12f));
        const float xl = sampleToX (pts.loopStart);
        g.fillRect (juce::Rectangle<float> (xl, (float) a.getY(), juce::jmax (0.0f, xe - xl), (float) a.getHeight()));
        if (pts.xfade > 0)
        {
            const float xx = sampleToX (pts.end - pts.xfade);
            g.setColour (juce::Colour (0xffff8a3d).withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float> (xx, (float) a.getY(), juce::jmax (0.0f, xe - xx), (float) a.getHeight()));
        }
    }
    g.setColour (Pal::panelHi);
    g.drawHorizontalLine ((int) cy, (float) a.getX(), (float) a.getRight());
    drawWave (juce::Colour (0xff6fd3ff));

    auto marker = [&] (int pos, juce::Colour c, const juce::String& txt)
    {
        const float x = sampleToX (pos);
        if (x < a.getX() - 1 || x > a.getRight() + 1) return;
        g.setColour (c);
        g.drawVerticalLine ((int) x, (float) a.getY(), (float) a.getBottom());
        g.fillRect (x - 3.0f, (float) a.getY(), 7.0f, 7.0f);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (txt, (int) x + 4, a.getY() + 2, 70, 14, juce::Justification::centredLeft);
    };
    marker (pts.start, juce::Colour (0xff3ddc84), "S");
    if (pts.loop) marker (pts.loopStart, juce::Colour (0xffffd23d), "LOOP");
    marker (pts.end, juce::Colour (0xffff4d5e), "E");

    g.setColour (Pal::dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ((vZoom > 1.01f ? T ("height x") + juce::String (vZoom, 1) + "  |  " : juce::String())
                + juce::String (len / sample->sampleRate, 2) + " s  |  " + juce::String (len) + T (" samples"),
                a.reduced (6, 2), juce::Justification::bottomRight);
}

void WaveEditor::resized()
{
    auto top = getLocalBounds().removeFromTop (28);
    sampleMode.setBounds (top.removeFromLeft (100).reduced (1, 0));
    loopMode.setBounds (top.removeFromLeft (90).reduced (1, 0));
    fitButton.setBounds (top.removeFromLeft (50).reduced (1, 0));
    top.removeFromLeft (8);
    loopToggle.setBounds (top.removeFromLeft (64));
    zeroToggle.setBounds (top.removeFromLeft (84));
    for (int i = 0; i < 3; ++i)
    {
        labels[i].setBounds (top.removeFromLeft (i == 1 ? 70 : 42));
        boxes[i].setBounds (top.removeFromLeft (150).reduced (2, 2));
    }
    auto row2 = getLocalBounds().withTrimmedTop (31).removeFromTop (28);
    autoButton.setBounds (row2.removeFromLeft (100).reduced (1, 2));
    autoAllButton.setBounds (row2.removeFromLeft (150).reduced (1, 2));
    autoStartButton.setBounds (row2.removeFromLeft (90).reduced (1, 2));
    autoStartAllButton.setBounds (row2.removeFromLeft (150).reduced (1, 2));
    vFit.setBounds (row2.removeFromRight (90).reduced (1, 2));
    vPlus.setBounds (row2.removeFromRight (70).reduced (1, 2));
    vMinus.setBounds (row2.removeFromRight (70).reduced (1, 2));
    row2.removeFromLeft (6);
    for (auto [l, s] : { std::pair<juce::Label*, juce::Slider*> { &xfadeLabel, &xfadeMs }, { &minLabel, &minLoopMs }, { &maxLabel, &maxLoopMs } })
    {
        l->setBounds (row2.removeFromLeft (70));
        s->setBounds (row2.removeFromLeft (150).reduced (2, 2));
    }
    autoInfo.setBounds (row2.reduced (6, 0));
}

void WaveEditor::mouseDown (const juce::MouseEvent& e)
{
    if (sample == nullptr || ! waveArea().contains (e.getPosition())) return;
    drag = markerAt ((float) e.x);
    panning = drag == None && ! loopView;
    panStartView = viewStart;
    panStartX = (float) e.x;
}

void WaveEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (sample == nullptr) return;
    if (panning)
    {
        const double perPx = viewLen / juce::jmax (1, waveArea().getWidth());
        const double len = sample->length();
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, len - viewLen), panStartView - (e.x - panStartX) * perPx);
        repaint();
        return;
    }
    if (drag == None) return;
    auto np = pts;
    if (loopView)
    {
        // pomeranje sredine pomera End (spoj se posmatra na istom mestu)
        const double perPx = (double) seamHalf / (waveArea().getWidth() * 0.5);
        np.end = pts.end - (int) std::round ((e.x - panStartX) * perPx);
        panStartX = (float) e.x;
        if (np.end == pts.end) return;
        np.end = snap (np.end);
        if (np.end != pts.end) apply (np, false);
        return;
    }
    const int pos = snap ((int) std::round (xToSample ((float) e.x)));
    (drag == Start ? np.start : drag == LoopStart ? np.loopStart : np.end) = pos;
    apply (np, false);
}

void WaveEditor::mouseUp (const juce::MouseEvent&)
{
    if (drag != None && onChange) onChange (pts, true);
    drag = None;
    panning = false;
}

void WaveEditor::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (markerAt ((float) e.x) != None ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void WaveEditor::fitHeight()
{
    if (sample == nullptr) return;
    const int len = sample->length();
    const int i0 = juce::jlimit (0, len, (int) viewStart), i1 = juce::jlimit (i0, len, (int) (viewStart + viewLen));
    float pk = 0;
    for (int c = 0; c < juce::jmin (2, sample->audio().getNumChannels()); ++c)
        if (i1 > i0) pk = juce::jmax (pk, sample->audio().getMagnitude (c, i0, i1 - i0));
    vZoom = pk > 1.0e-6f ? juce::jlimit (1.0f, 1000.0f, 0.95f / pk) : 1.0f;
}

void WaveEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (sample == nullptr) return;
    const double factor = w.deltaY > 0 ? 0.8 : 1.25;
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())   // Ctrl + tockic: zum po visini
    {
        vZoom = juce::jlimit (1.0f, 1000.0f, vZoom * (w.deltaY > 0 ? 1.25f : 0.8f));
        repaint();
        return;
    }
    if (loopView)
    {
        seamHalf = juce::jlimit (16, 48000, (int) std::round (seamHalf * factor));
        repaint();
        return;
    }
    const double len = sample->length();
    const double at = xToSample ((float) e.x);
    const double newLen = juce::jlimit (64.0, juce::jmax (64.0, len), viewLen * factor);
    const double rel = (at - viewStart) / viewLen;
    viewLen = newLen;
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, len - viewLen), at - rel * viewLen);
    repaint();
}

MappingEditor::MappingEditor (LiveSamplerProcessor& p)
    : proc (p), mapView (p.keyboardState)
{
    setWantsKeyboardFocus (true);

    const char* viewNames[] = { "MAP", "SAMPLE / LOOP EDIT" };
    for (int b = 0; b < 2; ++b)
    {
        viewButtons[b].setButtonText (T (viewNames[b]));
        viewButtons[b].setColour (juce::TextButton::buttonOnColourId, Pal::master);
        viewButtons[b].getProperties().set ("fontSize", 12.0f);
        viewButtons[b].onClick = [this, b] { setWaveView (b == 1); };
        addAndMakeVisible (viewButtons[b]);
    }
    viewButtons[0].setToggleState (true, juce::dontSendNotification);
    addChildComponent (waveEditor);
    waveEditor.onAutoAll = [this]
    {
        if (inst == nullptr) return;
        auto z = zones;
        int found = 0, total = 0;
        for (auto& zz : z)
        {
            if (zz.sample < 0 || zz.sample >= (int) inst->samples.size()) continue;
            ++total;
            const auto& s = *inst->samples[(size_t) zz.sample];
            const auto r = AutoLoop::find (s.audio(), s.length(), s.sampleRate, waveEditor.autoSettings());
            if (! r.found) continue;
            zz.loop = true;
            zz.loopStart = r.start;
            zz.loopEnd = r.end;
            zz.loopXfade = r.crossfade;
            if (zz.sampleStart > r.start) zz.sampleStart = 0;
            ++found;
        }
        commit (z, true);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Auto loop",
            T ("Loop found for ") + juce::String (found) + T (" of ") + juce::String (total) + T (" zones. Save the map with the Save button."));
    };
    waveEditor.onAutoStartAll = [this]
    {
        if (inst == nullptr) return;
        auto z = zones;
        int changed = 0;
        for (auto& zz : z)
        {
            if (zz.sample < 0 || zz.sample >= (int) inst->samples.size()) continue;
            const auto& s = *inst->samples[(size_t) zz.sample];
            int st = AutoLoop::findOnset (s.audio(), s.length(), s.sampleRate);
            if (zz.loop && st >= zz.loopStart) st = 0;
            if (st != zz.sampleStart) { zz.sampleStart = juce::jmax (1, st); ++changed; }
        }
        commit (z, true);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Auto start",
            T ("Start moved to the note onset for ") + juce::String (changed) + T (" zones. Save the map with the Save button."));
    };
    waveEditor.onChange = [this] (const WaveEditor::Points& p, bool)
    {
        editSelected ([p] (Zone& z)
        {
            z.sampleStart = juce::jmax (1, p.start);
            z.sampleEnd = p.end;
            z.loop = p.loop;
            if (p.loop)
            {
                z.loopStart = p.loopStart;
                z.loopEnd = p.end;
                z.loopXfade = p.xfade;
            }
        }, true);
    };

    title.setText ("MAPPING", juce::dontSendNotification);
    title.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, Pal::master);
    addAndMakeVisible (title);

    for (int b = 0; b < kNumBlocks; ++b)
    {
        blockSel[b].setButtonText ("OSC" + juce::String (b + 1));
        blockSel[b].setColour (juce::TextButton::buttonOnColourId, Pal::part[b]);
        blockSel[b].getProperties().set ("fontSize", 10.5f);
        blockSel[b].onClick = [this, b] { setOsc (b * kMsPerOsc + osc % kMsPerOsc); };
        addAndMakeVisible (blockSel[b]);
    }
    for (int m = 0; m < kMsPerOsc; ++m)
    {
        msSel[m].setButtonText ("MS" + juce::String (m + 1));
        msSel[m].setColour (juce::TextButton::buttonOnColourId, Pal::accent);
        msSel[m].getProperties().set ("fontSize", 10.5f);
        msSel[m].onClick = [this, m] { setOsc ((osc / kMsPerOsc) * kMsPerOsc + m); };
        addAndMakeVisible (msSel[m]);
    }
    userLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    userLabel.setColour (juce::Label::textColourId, Pal::dim);
    userLabel.setText ("USER:", juce::dontSendNotification);
    addAndMakeVisible (userLabel);
    userBox.setTooltip (T ("Choose the user; buttons OSC1-8 and MS1-8 select which of its multisamples you map"));
    userBox.onChange = [this] { const int i = userBox.getSelectedItemIndex(); if (i >= 0 && i != userIdx) setUserOsc (i, 0); };
    addAndMakeVisible (userBox);

    instLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (instLabel);
    stateLabel.setFont (juce::FontOptions (13.0f));
    stateLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (stateLabel);

    undoButton.setButtonText (T ("Undo"));
    undoButton.setTooltip ("Ctrl+Z");
    undoButton.onClick = [this] { undo(); };
    saveButton.setButtonText (T ("Save"));
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
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Pikaso Workstation",
                T ("Cannot save the map (check that the folder is not write-protected)."));
        }
    };
    revertButton.setButtonText (T ("Discard changes"));
    revertButton.setTooltip (T ("Go back to the last saved map"));
    revertButton.onClick = [this]
    {
        if (instName.isEmpty()) return;
        dirtyNames.erase (instName);
        undoStack.clear();
        proc.reloadInstrument (instName, false);
    };
    resetButton.setButtonText (T ("Automatic"));
    resetButton.setTooltip (T ("Rebuild the map from the file (KMP/NKI/names). The saved map is deleted."));
    resetButton.onClick = [this]
    {
        if (instName.isEmpty() || inst == nullptr) return;
        inst->getMapFile().deleteFile();
        dirtyNames.erase (instName);
        undoStack.clear();
        proc.reloadInstrument (instName, true);
    };
    closeButton.setButtonText (T ("Close"));
    closeButton.onClick = [this] { if (onClose) onClose(); };

    toolsButton.setButtonText (T ("TOOLS"));
    toolsButton.setTooltip (T ("Trim samples on disk, normalize WAV, equalize zone levels"));
    toolsButton.onClick = [this] { toolsMenu(); };
    for (auto* b : { &undoButton, &saveButton, &revertButton, &resetButton, &closeButton, &toolsButton })
        addAndMakeVisible (b);

    // grafik
    mapView.onSelect = [this] (int i) { select (i); };
    mapView.onPreview = [this] (int i) { select (i); previewSelected(); };
    mapView.onToggle = [this] (int i)
    {
        auto s2 = sel;
        if (s2.empty() && selected >= 0) s2.insert (selected);
        if (s2.count (i)) s2.erase (i); else s2.insert (i);
        setSelection (s2, s2.count (i) ? i : (s2.empty() ? -1 : *s2.begin()));
    };
    mapView.onRange = [this] (int i)
    {
        // Shift: sve zone u pravougaoniku izmedju glavne i ove (tasteri i jacine)
        if (selected < 0 || selected >= (int) zones.size()) { select (i); return; }
        const auto& a = zones[(size_t) selected];
        const auto& b = zones[(size_t) i];
        const int lo = juce::jmin (a.lowKey, b.lowKey), hi = juce::jmax (a.highKey, b.highKey);
        const int vl = juce::jmin (a.lowVel, b.lowVel), vh = juce::jmax (a.highVel, b.highVel);
        auto s2 = sel;
        for (int k = 0; k < (int) zones.size(); ++k)
        {
            const auto& z = zones[(size_t) k];
            if (z.lowKey >= lo && z.highKey <= hi && z.lowVel >= vl && z.highVel <= vh) s2.insert (k);
        }
        s2.insert (selected);
        s2.insert (i);
        setSelection (s2, i);
    };
    mapView.onBox = [this] (std::vector<int> hits, bool add)
    {
        std::set<int> s2 = add ? sel : std::set<int>();
        if (add && s2.empty() && selected >= 0) s2.insert (selected);
        for (int k : hits) s2.insert (k);
        setSelection (s2, hits.front());
    };
    mapView.onChange = [this] (const std::vector<Zone>& z, bool afterChange)
    {
        if (! afterChange) { undoStack.push_back (zones); return; }
        commit (z, false);
    };
    addAndMakeVisible (mapView);

    // tabela
    table.setModel (this);
    table.setMultipleSelectionEnabled (true);   // Ctrl / Shift + klik u tabeli
    table.setColour (juce::ListBox::backgroundColourId, Pal::bg);
    table.setRowHeight (24);
    auto& h = table.getHeader();
    h.addColumn ("#", colNum, 36, 30, 50, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Sample", colSample, 250, 100, 600, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Root", colRoot, 70, 50, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Low", colLow, 70, 50, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("High", colHigh, 70, 50, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Velocity", colVel, 80, 60, 120, juce::TableHeaderComponent::notSortable);
    h.addColumn (T ("Tune"), colTune, 60, 50, 100, juce::TableHeaderComponent::notSortable);
    h.addColumn (T ("Level"), colGain, 70, 50, 100, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Loop", colLoop, 60, 40, 100, juce::TableHeaderComponent::notSortable);
    h.setColour (juce::TableHeaderComponent::backgroundColourId, Pal::panelHi);
    h.setColour (juce::TableHeaderComponent::textColourId, Pal::text);
    addAndMakeVisible (table);

    // inspektor
    sampleInfo.setFont (juce::FontOptions (13.0f));
    sampleInfo.setJustificationType (juce::Justification::topLeft);
    sampleInfo.setColour (juce::Label::textColourId, Pal::text);
    addAndMakeVisible (sampleInfo);

    const char* labels[] = { "Root", "Low key", "High key", "Vel lo", "Vel hi", "Tune", "Level", "Pan" };
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

    // tasteri: vise izabranih zona se pomera za istu razliku (ne dobijaju svi isti taster)
    auto keyDelta = [this] (int Zone::* field, int value)
    {
        if (selected < 0 || selected >= (int) zones.size()) return 0;
        return value - zones[(size_t) selected].*field;
    };
    rootSlider.onValueChange    = [this, keyDelta]
    {
        const int d = keyDelta (&Zone::rootKey, (int) rootSlider.getValue());
        editSelected ([d] (Zone& z) { z.rootKey = juce::jlimit (0, 127, z.rootKey + d); });
    };
    lowKeySlider.onValueChange  = [this, keyDelta]
    {
        const int d = keyDelta (&Zone::lowKey, (int) lowKeySlider.getValue());
        editSelected ([d] (Zone& z) { z.lowKey = juce::jlimit (0, 127, z.lowKey + d); z.highKey = juce::jmax (z.highKey, z.lowKey); });
    };
    highKeySlider.onValueChange = [this, keyDelta]
    {
        const int d = keyDelta (&Zone::highKey, (int) highKeySlider.getValue());
        editSelected ([d] (Zone& z) { z.highKey = juce::jlimit (0, 127, z.highKey + d); z.lowKey = juce::jmin (z.lowKey, z.highKey); });
    };
    lowVelSlider.onValueChange  = [this] { editSelected ([this] (Zone& z) { z.lowVel = (int) lowVelSlider.getValue(); z.highVel = juce::jmax (z.highVel, z.lowVel); }); };
    highVelSlider.onValueChange = [this] { editSelected ([this] (Zone& z) { z.highVel = (int) highVelSlider.getValue(); z.lowVel = juce::jmin (z.lowVel, z.highVel); }); };
    tuneSlider.onValueChange    = [this] { editSelected ([this] (Zone& z) { z.tuneCents = (float) tuneSlider.getValue(); }); };
    gainSlider.onValueChange    = [this] { editSelected ([this] (Zone& z) { z.gainDb = (float) gainSlider.getValue(); }); };
    panSlider.onValueChange     = [this] { editSelected ([this] (Zone& z) { z.pan = (float) panSlider.getValue(); }); };

    enabledToggle.setButtonText (T ("Enabled"));
    loopToggle.setButtonText ("Loop");
    fixedToggle.setButtonText (T ("Fixed pitch"));
    fixedToggle.setTooltip (T ("The sample plays at the same pitch on all keys (drums, effects)"));
    enabledToggle.onClick = [this] { editSelected ([this] (Zone& z) { z.enabled = enabledToggle.getToggleState(); }, false); };
    loopToggle.onClick    = [this]
    {
        const bool on = loopToggle.getToggleState();
        editSelected ([this, on] (Zone& z)
        {
            z.loop = on;
            // sempl bez loop-a: napravi loop od pocetka do kraja (doteraj u SAMPLE / LOOP EDIT)
            if (on && ! z.hasOwnLoop() && inst != nullptr && z.sample >= 0 && z.sample < (int) inst->samples.size()
                && ! inst->samples[(size_t) z.sample]->hasLoop)
            {
                const auto& s = *inst->samples[(size_t) z.sample];
                z.loopStart = z.sampleStart > 0 ? z.sampleStart : s.startOffset;
                z.loopEnd = s.endPoint > 0 ? s.endPoint : s.length();
            }
        }, false);
    };
    fixedToggle.onClick   = [this] { editSelected ([this] (Zone& z) { z.fixedPitch = fixedToggle.getToggleState(); }, false); };
    for (auto* t : { &enabledToggle, &loopToggle, &fixedToggle })
        addAndMakeVisible (t);

    duplicateButton.setButtonText ("Duplicate zone");
    duplicateButton.onClick = [this]
    {
        const auto list = selectedList();
        if (list.empty()) return;
        auto z = zones;
        std::set<int> added;
        for (int i : list) z.push_back (z[(size_t) i]);   // kopije idu na kraj
        for (int k = 0; k < (int) list.size(); ++k) added.insert ((int) zones.size() + k);
        commit (z, true);
        setSelection (added, *added.begin());
    };
    playButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb6 Play"));
    playButton.setTooltip (T ("Listen to the sample of the selected zone (also: double-click a zone or a row, or Space). Click again to stop."));
    playButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f7d4a));
    playButton.onClick = [this] { previewSelected(); };
    addAndMakeVisible (playButton);
    deleteButton.setButtonText (T ("Delete zone"));
    deleteButton.onClick = [this]
    {
        auto list = selectedList();
        if (list.empty()) return;
        const int first = list.front();
        auto z = zones;
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if (*it < (int) z.size()) z.erase (z.begin() + *it);
        sel.clear();
        commit (z, true);
        select (juce::jmin (first, (int) zones.size() - 1));
    };
    octDownButton.setButtonText (T ("All − octave"));
    octDownButton.setTooltip (T ("Move all zones and root notes one octave down"));
    octUpButton.setButtonText (T ("All + octave"));
    octUpButton.setTooltip (T ("Move all zones and root notes one octave up"));
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

    rangesButton.setButtonText (T ("Ranges by root"));
    rangesButton.setTooltip (T ("Split the keys between zones again by their root notes (for each velocity layer)"));
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

    hint.setText (T ("Click a zone to select it, drag it or its edges. Ctrl + click: add / remove, Shift + click: all in between, drag on an empty spot: box, Ctrl + A: all. Several selected move and change together."),
                  juce::dontSendNotification);
    hint.setFont (juce::FontOptions (13.0f));
    hint.setMinimumHorizontalScale (1.0f);
    hint.setColour (juce::Label::textColourId, Pal::dim);
    addAndMakeVisible (hint);

    setUserOsc (0, 0);
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
void MappingEditor::setUserOsc (int userIndex, int o)
{
    userIdx = userIndex;
    osc = juce::jlimit (0, kNumOsc - 1, o);
    for (int b = 0; b < kNumBlocks; ++b)
        blockSel[b].setToggleState (b == osc / kMsPerOsc, juce::dontSendNotification);
    for (int m = 0; m < kMsPerOsc; ++m)
    {
        msSel[m].setToggleState (m == osc % kMsPerOsc, juce::dontSendNotification);
        msSel[m].setColour (juce::TextButton::buttonOnColourId, Pal::part[osc / kMsPerOsc]);
    }
    refresh();
}

void MappingEditor::refresh()
{
    juce::String name;
    if (userIdx >= 0 && userIdx < proc.getNumUsers())
    {
        const auto& u = proc.getUser (userIdx);
        name = u.osc[(size_t) osc].instrument;
        const auto names = proc.getUserNames();
        if (names != shownUsers)
        {
            shownUsers = names;
            userBox.clear (juce::dontSendNotification);
            userBox.addItemList (names, 1);
        }
        userBox.setSelectedItemIndex (userIdx, juce::dontSendNotification);
        for (int b = 0; b < kNumBlocks; ++b)
        {
            bool any = false;
            for (int m = 0; m < kMsPerOsc; ++m) any = any || u.osc[(size_t) (b * kMsPerOsc + m)].instrument.isNotEmpty();
            blockSel[b].setAlpha (any ? 1.0f : 0.4f);
        }
        for (int m = 0; m < kMsPerOsc; ++m)
            msSel[m].setAlpha (u.osc[(size_t) ((osc / kMsPerOsc) * kMsPerOsc + m)].instrument.isNotEmpty() ? 1.0f : 0.4f);
        if (name.isNotEmpty() && proc.getLoadedInstrument (name) == nullptr)
            proc.ensureLoaded (name);   // ucitaj i instrumente koji nisu u performansu
    }
    auto cur = proc.getLoadedInstrument (name);

    if (name != instName)
    {
        instName = name;
        undoStack.clear();
        selected = -1;
        sel.clear();
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
        instLabel.setText (T ("(no instrument on this part)"), juce::dontSendNotification);
    else
        instLabel.setText (instName + (inst != nullptr ? "   [" + sourceTypeName (inst->type) + "]" : juce::String()),
                           juce::dontSendNotification);

    juce::String state;
    juce::Colour col = Pal::dim;
    if (instName.isNotEmpty() && inst == nullptr)
        state = T ("Loading...");
    else if (dirtyNames.count (instName) > 0)
    {
        state = T ("● Unsaved changes");
        col = juce::Colour (0xffffb340);
    }
    else if (inst != nullptr)
        state = inst->fromMapFile ? T ("Saved map") : T ("Automatic map");

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
    mapView.setZones (zones, selected, proc.getC3Is60(), sel);
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
    for (int i : selectedList())
        if (i < (int) z.size()) fn (z[(size_t) i]);   // ista izmena za sve izabrane zone
    commit (z, true, coalesce);
}

std::vector<int> MappingEditor::selectedList() const
{
    std::vector<int> v;
    for (int i : sel) if (i >= 0 && i < (int) zones.size()) v.push_back (i);
    if (v.empty() && selected >= 0 && selected < (int) zones.size()) v.push_back (selected);
    return v;
}

void MappingEditor::setSelection (std::set<int> s, int primary)
{
    std::set<int> clean;
    for (int i : s) if (i >= 0 && i < (int) zones.size()) clean.insert (i);
    sel = clean;
    selected = clean.count (primary) ? primary : (clean.empty() ? -1 : *clean.begin());
    if (sel.size() <= 1) sel.clear();
    syncSelectionUI();
}

void MappingEditor::syncSelectionUI()
{
    {
        const juce::ScopedValueSetter<bool> svs (updatingUI, true);
        juce::SparseSet<int> rows;
        for (int i : selectedList()) rows.addRange ({ i, i + 1 });
        table.setSelectedRows (rows, juce::dontSendNotification);
    }
    if (selected >= 0)
        table.scrollToEnsureRowIsOnscreen (selected);
    mapView.setZones (zones, selected, proc.getC3Is60(), sel);
    updateInspector();
}

void MappingEditor::select (int index)
{
    sel.clear();
    selected = (index >= 0 && index < (int) zones.size()) ? index : -1;
    syncSelectionUI();
}

void MappingEditor::setWaveView (bool w)
{
    waveView = w;
    viewButtons[0].setToggleState (! w, juce::dontSendNotification);
    viewButtons[1].setToggleState (w, juce::dontSendNotification);
    mapView.setVisible (! w);
    waveEditor.setVisible (w);
    resized();
    updateWave();
}

void MappingEditor::updateWave()
{
    if (! waveView) return;
    if (inst == nullptr || selected < 0 || selected >= (int) zones.size())
    {
        waveEditor.clear();
        return;
    }
    const auto& z = zones[(size_t) selected];
    if (z.sample < 0 || z.sample >= (int) inst->samples.size())
    {
        waveEditor.clear();
        return;
    }
    auto s = inst->samples[(size_t) z.sample];
    const int len = s->length();
    WaveEditor::Points p;
    p.start = z.sampleStart > 0 ? z.sampleStart : s->startOffset;
    const int endDefault = s->endPoint > 0 ? s->endPoint : len;
    p.loop = z.loop && (z.hasOwnLoop() || s->hasLoop);
    p.loopStart = z.hasOwnLoop() ? z.loopStart : (s->hasLoop ? s->loopStart : p.start);
    p.end = p.loop ? (z.hasOwnLoop() ? z.loopEnd : s->loopEnd) : (z.sampleEnd > 0 ? z.sampleEnd : endDefault);
    p.end = juce::jlimit (32, len, p.end);
    p.xfade = z.hasOwnLoop() ? z.loopXfade : 0;
    waveEditor.setSample (s, p);
}

void MappingEditor::updateInspector()
{
    updateWave();
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
        sampleInfo.setText (T ("Select a zone in the graph or in the table."), juce::dontSendNotification);
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
        loopToggle.setEnabled (true);
        juce::String info;
        info << s.displayName << "\n"
             << juce::String (s.lengthSeconds(), 2) << " s  |  " << juce::String (s.sampleRate / 1000.0, 1) << " kHz  |  "
             << (z.hasOwnLoop() ? T ("own zone loop") : (s.hasLoop ? T ("has loop") : T ("no loop"))) << "\n"
             << s.id;
        if (sel.size() > 1)
            info = juce::String ((int) sel.size()) + T (" zones selected: changes apply to all (keys move by the same amount)\n") + info;
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
                            + (z.enabled ? juce::String() : T ("  (disabled)")); break;
        case colRoot:   t = noteName (z.rootKey) + (z.fixedPitch ? " *" : ""); break;
        case colLow:    t = noteName (z.lowKey); break;
        case colHigh:   t = noteName (z.highKey); break;
        case colVel:    t = juce::String (z.lowVel) + "-" + juce::String (z.highVel); break;
        case colTune:   t = juce::String (juce::roundToInt (z.tuneCents)); break;
        case colGain:   t = juce::String (z.gainDb, 1); break;
        case colLoop:
        {
            const bool has = z.hasOwnLoop() || (z.sample >= 0 && z.sample < (int) inst->samples.size() && inst->samples[(size_t) z.sample]->hasLoop);
            t = ! has ? "-" : (z.loop ? T ("yes") : T ("off"));
            break;
        }
        default: break;
    }
    g.drawText (t, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

void MappingEditor::previewSelected()
{
    // isti sempl jos svira: drugi klik zaustavlja
    static juce::uint32 lastStart = 0;
    static int lastZone = -1;
    if (selected < 0 || selected >= (int) zones.size() || inst == nullptr) { proc.stopPreview(); return; }
    const auto& z = zones[(size_t) selected];
    if (z.sample < 0 || z.sample >= (int) inst->samples.size()) return;
    const auto now = juce::Time::getMillisecondCounter();
    const auto& smp = inst->samples[(size_t) z.sample];
    if (lastZone == selected && now - lastStart < (juce::uint32) (smp->lengthSeconds() * 1000.0))
    {
        proc.stopPreview();
        lastZone = -1;
        return;
    }
    proc.previewSample (smp);
    lastZone = selected;
    lastStart = now;
}

void MappingEditor::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    select (row);
    previewSelected();
}

void MappingEditor::selectedRowsChanged (int lastRowSelected)
{
    if (updatingUI)
        return;
    // tabela: Ctrl / Shift + klik bira vise zona
    std::set<int> s2;
    const auto rows = table.getSelectedRows();
    for (int k = 0; k < rows.size(); ++k) s2.insert (rows[k]);
    sel = s2.size() > 1 ? s2 : std::set<int>();
    selected = lastRowSelected >= 0 ? lastRowSelected : (s2.empty() ? -1 : *s2.begin());
    mapView.setZones (zones, selected, proc.getC3Is60(), sel);
    updateInspector();
}

bool MappingEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        undo();
        return true;
    }
    if (k == juce::KeyPress::spaceKey)
    {
        previewSelected();
        return true;
    }
    if (k == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
    {
        std::set<int> all;
        for (int i = 0; i < (int) zones.size(); ++i) all.insert (i);
        setSelection (all, selected >= 0 ? selected : 0);
        return true;
    }
    if (k == juce::KeyPress::escapeKey && ! sel.empty())
    {
        select (selected);
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
    title.setBounds (header.removeFromLeft (112));
    for (auto& b : blockSel)
        b.setBounds (header.removeFromLeft (38).reduced (1, 0));
    header.removeFromLeft (10);
    for (auto& b : msSel)
        b.setBounds (header.removeFromLeft (36).reduced (1, 0));
    header.removeFromLeft (10);
    closeButton.setBounds (header.removeFromRight (80));
    header.removeFromRight (4);
    resetButton.setBounds (header.removeFromRight (104));
    header.removeFromRight (4);
    revertButton.setBounds (header.removeFromRight (124));
    header.removeFromRight (4);
    saveButton.setBounds (header.removeFromRight (92));
    header.removeFromRight (4);
    undoButton.setBounds (header.removeFromRight (80));
    header.removeFromRight (4);
    toolsButton.setBounds (header.removeFromRight (70));

    r.removeFromTop (6);
    auto infoRow = r.removeFromTop (24);
    userLabel.setBounds (infoRow.removeFromLeft (44));
    userBox.setBounds (infoRow.removeFromLeft (230).reduced (0, 1));
    infoRow.removeFromLeft (10);
    stateLabel.setBounds (infoRow.removeFromRight (infoRow.getWidth() * 2 / 5));
    instLabel.setBounds (infoRow);
    hint.setVisible (false);
    r.removeFromTop (4);
    {
        auto vb = r.removeFromTop (24);
        viewButtons[0].setBounds (vb.removeFromLeft (90).reduced (1, 0));
        viewButtons[1].setBounds (vb.removeFromLeft (170).reduced (1, 0));
        r.removeFromTop (4);
    }
    if (waveView) waveEditor.setBounds (r.removeFromTop (juce::jlimit (180, 290, r.getHeight() - 10 - 286)));
    else          mapView.setBounds (r.removeFromTop (juce::jlimit (110, 172, r.getHeight() - 10 - 286)));   // dole mora da stane ceo inspektor
    r.removeFromTop (10);

    auto inspector = r.removeFromRight (420);
    r.removeFromRight (10);
    table.setBounds (r);

    sampleInfo.setBounds (inspector.removeFromTop (46));
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
    const int b1w = b1.getWidth() / 3;
    playButton.setBounds (b1.removeFromLeft (b1w).reduced (2, 0));
    duplicateButton.setBounds (b1.removeFromLeft (b1w).reduced (2, 0));
    deleteButton.setBounds (b1.reduced (2, 0));
    inspector.removeFromTop (6);
    auto b2 = inspector.removeFromTop (30);
    const int bw = b2.getWidth() / 3;
    octDownButton.setBounds (b2.removeFromLeft (bw).reduced (2, 0));
    octUpButton.setBounds (b2.removeFromLeft (bw).reduced (2, 0));
    rangesButton.setBounds (b2.reduced (2, 0));
}


//==============================================================================
//  ALATI: skracivanje WAV fajlova na disku, normalizacija, izjednacavanje jacine zona
//==============================================================================
void MappingEditor::toolsMenu()
{
    const bool wav = inst != nullptr && inst->type == SourceType::WavFolder;
    juce::PopupMenu m;
    m.addSectionHeader (T ("On disk (WAV, with confirmation)"));
    m.addItem (1, T ("Trim the selected zone (deletes before Start and after End / Loop end)"), wav && selected >= 0);
    m.addItem (2, T ("Trim all zones"), wav);
    m.addItem (3, T ("Normalize WAV files (same gain for all, dynamics stay)"), wav);
    m.addSeparator();
    m.addSectionHeader ("Delete zones");
    m.addItem (6, sel.size() > 1 ? "Delete the " + juce::String ((int) sel.size()) + " selected zones (and their WAV files)..." : juce::String ("Delete the selected zone (and its WAV file)..."), selected >= 0);
    m.addItem (7, "Delete zones in a key range, e.g. C5-C7 (and their WAV files)...", inst != nullptr && ! zones.empty());
    m.addSeparator();
    m.addSectionHeader (T ("In the map (files untouched)"));
    m.addItem (4, T ("Equalize zone levels (per velocity layer)"), inst != nullptr);
    m.addItem (5, T ("Reset the level of all zones to 0 dB"), inst != nullptr);
    {
        // round robin: zone koje dele isti taster i jacinu sviraju naizmenicno
        int rrZones = 0;
        for (size_t a = 0; a < zones.size(); ++a)
        {
            const auto& za = zones[a];
            if (! za.enabled) continue;
            for (size_t b = 0; b < zones.size(); ++b)
                if (a != b && zones[b].enabled && zones[b].matches (za.rootKey, za.lowVel) && za.matches (za.rootKey, za.lowVel)
                    && zones[b].lowKey == za.lowKey && zones[b].highKey == za.highKey && zones[b].lowVel == za.lowVel && zones[b].highVel == za.highVel)
                { ++rrZones; break; }
        }
        m.addSeparator();
        m.addSectionHeader (T ("Round robin (zones on the same keys and velocities alternate): ") + juce::String (rrZones) + T (" zones"));
        const bool rnd = inst != nullptr && inst->rrRandom;
        m.addItem (8, T ("In order (1, 2, 3, 1...)"), inst != nullptr, ! rnd);
        m.addItem (9, T ("Random (never the same twice in a row)"), inst != nullptr, rnd);
    }
    if (! wav) m.addItem (99, T ("Disk tools work only for WAV folders"), false);
    juce::Component::SafePointer<MappingEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (toolsButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 1 || r == 2) safe->trimToDisk (r == 1);
        if (r == 3) safe->normalizeFiles();
        if (r == 6) safe->deleteZones (safe->selectedList(), true, true);
        if (r == 7) safe->askDeleteRange();
        if (r == 4) safe->equalizeZones();
        if (r == 8 || r == 9)
        {
            safe->proc.setRoundRobinRandom (safe->instName, r == 9);
            safe->dirtyNames.insert (safe->instName);
            safe->refresh();
        }
        if (r == 5)
        {
            auto z = safe->zones;
            for (auto& zz : z) zz.gainDb = 0.0f;
            safe->commit (z, true);
        }
    });
}

// brisanje zona iz mape i (za WAV foldere) njihovih fajlova sa diska (idu u Recycle Bin)
void MappingEditor::askDeleteRange()
{
    if (inst == nullptr) return;
    auto* aw = new juce::AlertWindow ("Delete zones in a key range",
        "Zones whose root note is in this range are deleted from the map" + juce::String (inst->type == SourceType::WavFolder ? " and their WAV files go to the Recycle Bin." : "."),
        juce::MessageBoxIconType::WarningIcon);
    aw->addTextEditor ("from", "C5", "From note:");
    aw->addTextEditor ("to", "C7", "To note:");
    aw->addComboBox ("keys", { "The nearest remaining zone takes their keys", "Their keys stay silent" }, "Keys:");
    if (inst->type == SourceType::WavFolder)
        aw->addComboBox ("files", { "Delete the WAV files too (Recycle Bin)", "Only remove from the map" }, "Files:");
    aw->addButton ("Delete", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MappingEditor> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int ok)
    {
        if (safe == nullptr || ok == 0 || safe->inst == nullptr) return;
        const bool c3 = safe->proc.getC3Is60();
        auto parse = [c3] (juce::String t)
        {
            t = t.trim();
            if (t.containsOnly ("0123456789")) return t.getIntValue();
            int note = -1, layer = 0;
            return SampleLibrary::parseFileName (t, c3, note, layer) ? note : -1;
        };
        int lo = parse (aw->getTextEditorContents ("from")), hi = parse (aw->getTextEditorContents ("to"));
        if (lo < 0 || hi < 0) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Delete zones", "Write the notes like C5 and C7 (or MIDI numbers)."); return; }
        if (lo > hi) std::swap (lo, hi);
        std::vector<int> which;
        for (int i = 0; i < (int) safe->zones.size(); ++i)
            if (safe->zones[(size_t) i].rootKey >= lo && safe->zones[(size_t) i].rootKey <= hi) which.push_back (i);
        if (which.empty()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Delete zones", "No zone has its root note in this range."); return; }
        const bool take = aw->getComboBoxComponent ("keys")->getSelectedItemIndex() == 0;
        auto* fb = aw->getComboBoxComponent ("files");
        safe->deleteZones (which, take, fb != nullptr && fb->getSelectedItemIndex() == 0);
    }), true);
}

void MappingEditor::deleteZones (std::vector<int> which, bool neighboursTakeKeys, bool deleteFiles)
{
    if (inst == nullptr || which.empty()) return;
    std::sort (which.begin(), which.end());
    which.erase (std::unique (which.begin(), which.end()), which.end());
    std::set<int> del (which.begin(), which.end());
    std::vector<Zone> keep;
    std::vector<Zone> gone;
    for (int i = 0; i < (int) zones.size(); ++i)
        (del.count (i) ? gone : keep).push_back (zones[(size_t) i]);
    if (gone.empty()) return;

    // fajlovi koje vise nijedna zona ne koristi
    juce::Array<juce::File> files;
    const bool wav = inst->type == SourceType::WavFolder;
    if (deleteFiles && wav)
        for (auto& g : gone)
        {
            if (g.sample < 0 || g.sample >= (int) inst->samples.size()) continue;
            bool used = false;
            for (auto& k : keep) used = used || k.sample == g.sample;
            if (! used) files.addIfNotAlreadyThere (inst->source.getChildFile (inst->samples[(size_t) g.sample]->id));
        }

    if (neighboursTakeKeys)
        for (auto& g : gone)
            for (int key = g.lowKey; key <= g.highKey; ++key)
            {
                // najbliza preostala zona istog velocity opsega preuzima taster
                int best = -1, bestDist = 1000;
                for (int k = 0; k < (int) keep.size(); ++k)
                {
                    const auto& z = keep[(size_t) k];
                    if (! z.enabled || z.highVel < g.lowVel || z.lowVel > g.highVel) continue;
                    if (key >= z.lowKey && key <= z.highKey) { best = -1; bestDist = -1; break; }
                    const int d = key < z.lowKey ? z.lowKey - key : key - z.highKey;
                    if (d < bestDist) { bestDist = d; best = k; }
                }
                if (best >= 0)
                {
                    auto& z = keep[(size_t) best];
                    z.lowKey = juce::jmin (z.lowKey, key);
                    z.highKey = juce::jmax (z.highKey, key);
                }
            }

    double bytes = 0;
    for (auto& f : files) bytes += (double) f.getSize();
    const auto msg = juce::String ((int) gone.size()) + " zones are removed from the map of\n" + instName
                   + (files.isEmpty() ? juce::String() : "\n\n" + juce::String (files.size()) + " WAV files (" + juce::String (bytes / (1024.0 * 1024.0), 1)
                                                         + " MB) go to the Recycle Bin.")
                   + (neighboursTakeKeys ? "\n\nThe nearest remaining zones take over their keys." : "\n\nTheir keys will be silent.")
                   + "\nThe map is saved right away. This applies to every user that uses this multisample.";
    juce::Component::SafePointer<MappingEditor> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Delete zones", msg, "Delete", "Cancel", nullptr,
        juce::ModalCallbackFunction::create ([safe, keep, files] (int ok)
        {
            if (safe == nullptr || ok == 0 || safe->inst == nullptr) return;
            auto& me = *safe;
            me.commit (keep, false);
            me.proc.saveMap (me.instName);
            me.dirtyNames.erase (me.instName);
            me.undoStack.clear();
            juce::StringArray failed;
            for (auto& f : files)
                if (! f.moveToTrash() && ! f.deleteFile()) failed.add (f.getFileName());
            me.proc.reloadInstrument (me.instName, false);
            if (! failed.isEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Delete zones", "These files could not be deleted:\n" + failed.joinIntoString ("\n"));
        }));
}

// upisuje deo [from, to) sempla u isti WAV (isti broj kanala, bitova i sample rate), sa loop tackama i root notom
bool MappingEditor::rewriteWav (const juce::File& f, const SampleData& s, int from, int to, float gain, int loopStart, int loopEnd, juce::String& err)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    int chans = 2, bits = 24;
    double sr = s.sampleRate;
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) { err = T ("cannot read ") + f.getFileName(); return false; }
        chans = juce::jlimit (1, 2, (int) r->numChannels);
        bits = juce::jlimit (16, 32, (int) r->bitsPerSample);
        if (r->usesFloatingPointData) bits = 32;
        sr = r->sampleRate;
    }
    const int len = s.length();
    from = juce::jlimit (0, len, from);
    to = juce::jlimit (from + 1, len, to);
    juce::AudioBuffer<float> out (chans, to - from);
    for (int c = 0; c < chans; ++c)
    {
        out.copyFrom (c, 0, s.audio(), juce::jmin (c, s.audio().getNumChannels() - 1), from, to - from);
        if (std::abs (gain - 1.0f) > 1.0e-6f) out.applyGain (c, 0, to - from, gain);
    }
    std::unordered_map<juce::String, juce::String> meta;
    if (s.fileRootKey >= 0) meta["MidiUnityNote"] = juce::String (s.fileRootKey);
    if (loopEnd > loopStart + 16)
    {
        meta["NumSampleLoops"] = "1";
        meta["Loop0Type"] = "0";
        meta["Loop0Start"] = juce::String (loopStart - from);
        meta["Loop0End"] = juce::String (loopEnd - from - 1);
    }
    const auto tmp = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tmp-pikaso.wav");
    tmp.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (tmp);
        juce::WavAudioFormat wavFmt;
        auto w = wavFmt.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (chans)
                                                 .withBitsPerSample (bits).withMetadataValues (meta));
        if (w == nullptr) { err = T ("cannot write ") + f.getFileName(); tmp.deleteFile(); return false; }
        w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    }
    if (! tmp.moveFileTo (f)) { err = T ("cannot replace ") + f.getFileName(); tmp.deleteFile(); return false; }
    return true;
}

void MappingEditor::trimToDisk (bool onlySelected)
{
    if (inst == nullptr || inst->type != SourceType::WavFolder) return;
    // za svaki sempl: najraniji Start (ili pocetak crossfade-a pred loop-om) i najkasniji End / Loop end svih zona koje ga koriste
    struct Cut { int from = std::numeric_limits<int>::max(), to = 0, loopStart = -1, loopEnd = -1; };
    std::map<int, Cut> cuts;
    for (int i = 0; i < (int) zones.size(); ++i)
    {
        if (onlySelected && i != selected) continue;
        const auto& z = zones[(size_t) i];
        if (z.sample < 0 || z.sample >= (int) inst->samples.size()) continue;
        const auto& smp = *inst->samples[(size_t) z.sample];
        const int len = smp.length();
        int ls = -1, le = -1;
        if (z.loop && z.hasOwnLoop()) { ls = z.loopStart; le = z.loopEnd; }
        else if (z.loop && smp.hasLoop) { ls = smp.loopStart; le = smp.loopEnd; }
        int from = juce::jmax (0, z.sampleStart);
        int to = le > 0 ? le : (z.sampleEnd > 0 ? z.sampleEnd : (smp.endPoint > 0 ? smp.endPoint : len));
        if (ls >= 0) from = juce::jmin (from, juce::jmax (0, ls - z.loopXfade));
        auto& c = cuts[z.sample];
        c.from = juce::jmin (c.from, from);
        c.to = juce::jmax (c.to, juce::jmin (len, to));
        if (ls >= 0 && c.loopStart < 0) { c.loopStart = ls; c.loopEnd = le; }
    }
    // koliko se stedi
    double saved = 0, total = 0;
    int files = 0;
    for (auto& [si, c] : cuts)
    {
        const auto& smp = *inst->samples[(size_t) si];
        const double perSample = (double) inst->source.getChildFile (smp.id).getSize() / juce::jmax (1, smp.length());
        total += (double) inst->source.getChildFile (smp.id).getSize();
        const int keep = juce::jmax (0, c.to - c.from);
        if (keep < smp.length() - 32) { saved += perSample * (smp.length() - keep); ++files; }
    }
    if (files == 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("Trim"), T ("Nothing to trim: the samples already start at Start and end at End / Loop end."));
        return;
    }
    const auto msg = T ("Permanently rewriting ") + juce::String (files) + T (" WAV files in\n") + inst->source.getFullPathName()
                   + T ("\n\nThe part before Start and after End (with a loop: after Loop end) is deleted. Frees about ")
                   + juce::String (saved / (1024.0 * 1024.0), 1) + " MB of " + juce::String (total / (1024.0 * 1024.0), 1) + " MB."
                   + T ("\n\nThis cannot be undone.");
    juce::Component::SafePointer<MappingEditor> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("Trim samples on disk"), msg, T ("Rewrite"), T ("Cancel"), nullptr,
        juce::ModalCallbackFunction::create ([safe, cuts] (int ok)
        {
            if (safe == nullptr || ok == 0 || safe->inst == nullptr) return;
            auto& me = *safe;
            juce::StringArray errors;
            auto z = me.zones;
            for (auto& [si, c] : cuts)
            {
                const auto& smp = *me.inst->samples[(size_t) si];
                if (c.to - c.from >= smp.length() - 32) continue;
                juce::String err;
                if (! me.rewriteWav (me.inst->source.getChildFile (smp.id), smp, c.from, c.to, 1.0f, c.loopStart, c.loopEnd, err)) { errors.add (err); continue; }
                // zone tog sempla: tacke se pomeraju za odseceni pocetak
                for (auto& zz : z)
                {
                    if (zz.sample != si) continue;
                    if (zz.sampleStart > 0) zz.sampleStart = juce::jmax (1, zz.sampleStart - c.from);
                    if (zz.sampleEnd > 0) zz.sampleEnd = juce::jmax (1, zz.sampleEnd - c.from);
                    if (zz.hasOwnLoop()) { zz.loopStart -= c.from; zz.loopEnd -= c.from; }
                }
            }
            me.commit (z, false);
            me.proc.saveMap (me.instName);      // mapa mora da odgovara novim fajlovima
            me.dirtyNames.erase (me.instName);
            me.undoStack.clear();
            me.proc.reloadInstrument (me.instName, false);
            juce::AlertWindow::showMessageBoxAsync (errors.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                T ("Trim"), errors.isEmpty() ? T ("Done. The samples are trimmed and the map is saved.") : T ("Some files were not rewritten:\n") + errors.joinIntoString ("\n"));
        }));
}

void MappingEditor::normalizeFiles()
{
    if (inst == nullptr || inst->type != SourceType::WavFolder || inst->samples.empty()) return;
    float peak = 0.0f;
    for (auto& sp : inst->samples)
        for (int c = 0; c < sp->audio().getNumChannels(); ++c)
            peak = juce::jmax (peak, sp->audio().getMagnitude (c, 0, sp->length()));
    if (peak < 1.0e-6f) return;
    const float gain = juce::Decibels::decibelsToGain (-0.3f) / peak;
    const float gainDb = juce::Decibels::gainToDecibels (gain);
    if (std::abs (gainDb) < 0.1f)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("Normalize"), T ("The samples are already normalized (the loudest is at -0.3 dB)."));
        return;
    }
    juce::Component::SafePointer<MappingEditor> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("Normalize WAV"),
        T ("Permanently rewriting ") + juce::String ((int) inst->samples.size()) + T (" WAV files with gain ")
            + (gainDb > 0 ? "+" : "") + juce::String (gainDb, 1) + T (" dB (the loudest sample goes to -0.3 dB).\nAll samples get the same gain, so the difference between soft and loud layers stays.\n\nThis cannot be undone."),
        T ("Rewrite"), T ("Cancel"), nullptr,
        juce::ModalCallbackFunction::create ([safe, gain] (int ok)
        {
            if (safe == nullptr || ok == 0 || safe->inst == nullptr) return;
            auto& me = *safe;
            juce::StringArray errors;
            for (auto& sp : me.inst->samples)
            {
                juce::String err;
                if (! me.rewriteWav (me.inst->source.getChildFile (sp->id), *sp, 0, sp->length(), gain,
                                     sp->hasLoop ? sp->loopStart : -1, sp->hasLoop ? sp->loopEnd : -1, err))
                    errors.add (err);
            }
            me.proc.reloadInstrument (me.instName, false);
            juce::AlertWindow::showMessageBoxAsync (errors.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                T ("Normalize"), errors.isEmpty() ? T ("Done.") : T ("Some files were not rewritten:\n") + errors.joinIntoString ("\n"));
        }));
}

// u mapi: zone istog velocity sloja dobijaju jacinu tako da im glasnoca (pocetak tona, 0.3 s) bude ista
void MappingEditor::equalizeZones()
{
    if (inst == nullptr) return;
    auto z = zones;
    std::map<std::pair<int, int>, std::vector<std::pair<size_t, float>>> groups;
    for (size_t i = 0; i < z.size(); ++i)
    {
        if (z[i].sample < 0 || z[i].sample >= (int) inst->samples.size()) continue;
        const auto& smp = *inst->samples[(size_t) z[i].sample];
        const int len = smp.length();
        const int on = z[i].sampleStart > 0 ? z[i].sampleStart : AutoLoop::findOnset (smp.audio(), len, smp.sampleRate);
        const int n = juce::jmin (len - on, (int) (smp.sampleRate * 0.3));
        if (n <= 0) continue;
        float rms = 0;
        for (int c = 0; c < juce::jmin (2, smp.audio().getNumChannels()); ++c) rms += smp.audio().getRMSLevel (c, on, n) * 0.5f;
        if (rms > 1.0e-6f) groups[{ z[i].lowVel, z[i].highVel }].push_back ({ i, juce::Decibels::gainToDecibels (rms) });
    }
    int changed = 0;
    for (auto& [k, v] : groups)
    {
        if (v.size() < 2) continue;
        std::vector<float> lv;
        for (auto& p : v) lv.push_back (p.second);
        std::sort (lv.begin(), lv.end());
        const float target = lv[lv.size() / 2];
        for (auto& [i, db] : v)
        {
            z[i].gainDb = juce::jlimit (-12.0f, 12.0f, std::round ((target - db) * 10.0f) / 10.0f);
            ++changed;
        }
    }
    commit (z, true);
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("Equalize levels"),
        T ("Level set for ") + juce::String (changed) + T (" zones (Level column). Soft and loud layers stay different.\nListen, then click Save (or Undo)."));
}

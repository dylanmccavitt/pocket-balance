#pragma once
#include "PluginProcessor.h"
#include <vector>

// All history and drawing live on the message thread. Audio only publishes small frames.
class WaveformView : public juce::Component
{
public:
    explicit WaveformView(PocketBalanceProcessor& p) : owner(p), history(2200)
    {
        setTitle("Live drum waveforms");
        mode.addItem("Kick + 808", 1); mode.addItem("Drum mix", 2);
        for (int i = 0; i < pocket::laneCount; ++i)
            mode.addItem(laneCaption(i), i + 3);
        time.addItem("1 second", 1); time.addItem("2 seconds", 2); time.addItem("4 seconds", 4);
        zoom.addItem("1x height", 1); zoom.addItem("2x height", 2);
        zoom.addItem("4x height", 4); zoom.addItem("8x height", 8);
        mode.setName("scope_view"); time.setName("scope_time"); zoom.setName("scope_zoom");
        observedMode = juce::jlimit(1, 18, int(owner.parameters.state.getProperty("scopeView", 1)));
        const int savedTime = int(owner.parameters.state.getProperty("scopeTime", 2));
        observedTime = savedTime == 1 || savedTime == 4 ? savedTime : 2;
        const int savedZoom = int(owner.parameters.state.getProperty("scopeZoom", 2));
        observedZoom = savedZoom == 1 || savedZoom == 4 || savedZoom == 8 ? savedZoom : 2;
        mode.setSelectedId(observedMode, juce::dontSendNotification);
        time.setSelectedId(observedTime, juce::dontSendNotification);
        zoom.setSelectedId(observedZoom, juce::dontSendNotification);
        mode.onChange = [this] { saveChoice(mode, observedMode, "scopeView"); };
        time.onChange = [this] { saveChoice(time, observedTime, "scopeTime"); };
        zoom.onChange = [this] { saveChoice(zoom, observedZoom, "scopeZoom"); };
        freeze.setClickingTogglesState(true);
        freeze.onClick = [this]
        {
            if (!freeze.getToggleState()) { count = 0; write = 0; }
            freeze.setButtonText(freeze.getToggleState() ? "Resume" : "Freeze"); repaint();
        };
        mode.setTooltip("View kick and 808 together, the final drum mix, or an individual input after its level and ducking.");
        time.setTooltip("How much audio history to display. This changes the picture only.");
        zoom.setTooltip("Magnify waveform height without changing the sound. Clipped drawing at higher zoom does not mean audio clipping.");
        freeze.setTooltip("Freeze the waveform for inspection. Audio keeps playing.");
        for (auto* c : std::initializer_list<juce::Component*> { &mode, &time, &zoom, &freeze }) addAndMakeVisible(c);
        owner.setScopeActive(true);
    }
    ~WaveformView() override { owner.setScopeActive(false); }

    void refresh()
    {
        refreshChoice(mode, observedMode, juce::jlimit(1, 18, int(owner.parameters.state.getProperty("scopeView", 1))));
        const int savedTime = int(owner.parameters.state.getProperty("scopeTime", 2));
        const int savedZoom = int(owner.parameters.state.getProperty("scopeZoom", 2));
        refreshChoice(time, observedTime, savedTime == 1 || savedTime == 4 ? savedTime : 2);
        refreshChoice(zoom, observedZoom, savedZoom == 1 || savedZoom == 4 || savedZoom == 8 ? savedZoom : 2);
        pocket::ScopeFrame frame;
        bool received = false;
        while (owner.popScopeFrame(frame))
        {
            received = true;
            if (freeze.getToggleState()) continue;
            if (count != 0 && (frame.seconds <= newest().seconds || frame.seconds - newest().seconds > 0.25))
            { count = 0; write = 0; }
            history[write] = frame; write = (write + 1) % history.size();
            count = std::min(count + 1, history.size());
        }
        if (received) lastReceived = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < pocket::laneCount; ++i)
        {
            const auto caption = laneCaption(i);
            if (mode.getItemText(i + 2) != caption) mode.changeItemText(i + 3, caption);
        }
        repaint();
    }

    void resized() override
    {
        const int w = getWidth();
        mode.setBounds(w - 570, 12, 192, 28);
        time.setBounds(w - 368, 12, 118, 28);
        zoom.setBounds(w - 240, 12, 118, 28);
        freeze.setBounds(w - 112, 12, 96, 28);
    }

    void paint(juce::Graphics& g) override
    {
        const juce::Colour bg(0xff0c1210), edge(0xff29372f), quiet(0xff99ada0);
        const juce::Colour green(0xffc3ec85), blue(0xff83cfe0), amber(0xffefb278);
        g.setColour(bg); g.fillRoundedRectangle(getLocalBounds().toFloat(), 10);
        g.setColour(edge); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 10, 1);
        label(g, "WAVEFORMS", { 16, 11, 125, 29 }, 12, green, true);
        const bool frozen = freeze.getToggleState();
        const bool streaming = juce::Time::getMillisecondCounterHiRes() - lastReceived < 500;
        label(g, frozen ? "FROZEN" : streaming ? "LIVE" : "WAITING FOR AUDIO",
              { 139, 12, 132, 27 }, 10, frozen ? amber : quiet, true);
        auto plot = juce::Rectangle<int>(100, 51, getWidth() - 124, getHeight() - 100);
        const bool pair = mode.getSelectedId() == 1;
        const float seconds = float(time.getSelectedId());
        const double end = count == 0 ? 0 : newest().seconds;
        const double start = end - double(seconds);
        for (int t = 0; t <= 4; ++t)
        {
            const int x = plot.getX() + plot.getWidth() * t / 4;
            g.setColour(edge.withAlpha(0.65f)); g.drawVerticalLine(x, float(plot.getY()), float(plot.getBottom()));
            label(g, t == 4 ? "now" : juce::String(seconds * (1 - float(t) / 4), 1) + "s",
                  { x - 26, getHeight() - 43, 52, 16 }, 10, quiet, false, juce::Justification::centred);
        }
        if (pair)
        {
            const int row = plot.getHeight() / 3;
            const auto kick = plot.withHeight(row - 3);
            const auto bass = plot.withY(plot.getY() + row).withHeight(row - 3);
            const auto reduction = plot.withY(plot.getY() + row * 2).withHeight(row - 3);
            label(g, "KICK", { 16, kick.getY(), 76, kick.getHeight() }, 11, green, true);
            label(g, "808", { 16, bass.getY(), 76, bass.getHeight() }, 11, blue, true);
            label(g, "DUCK", { 16, reduction.getY(), 76, reduction.getHeight() }, 11, amber, true);
            drawWave(g, kick, 17, green, start, seconds, 0.65f);
            drawWave(g, bass, 18, quiet, start, seconds, 0.24f);
            drawWave(g, bass, 19, blue, start, seconds, 0.78f);
            drawDuck(g, reduction, start, seconds, amber);
            label(g, "Gray: 808 before   /   Blue: after   /   Duck: 0-12 dB", { 16, getHeight() - 23, 370, 16 }, 10, quiet);
            juce::String state;
            if (frozen) state = "Frozen history - audio still plays";
            else if (owner.compare.load()) state = "Balance bypassed: ducking off";
            else if (owner.value("duck") < 0.05f) state = "Ducking off - raise Kick > 808 Space";
            else state = "Ducking " + juce::String(owner.duckReduction.load(), 1) + " dB";
            label(g, state, { 280, 13, getWidth() - 865, 26 }, 11, owner.value("duck") < 0.05f ? quiet : amber);
        }
        else
        {
            const int trace = mode.getSelectedId() == 2 ? 16 : mode.getSelectedId() - 3;
            label(g, trace == 16 ? "MIX" : "INPUT " + juce::String(trace + 1).paddedLeft('0', 2),
                  { 16, plot.getY(), 78, plot.getHeight() }, 11, trace == 16 ? green : blue, true);
            drawWave(g, plot, trace, trace == 16 ? green : blue, start, seconds, 0.7f);
            label(g, trace == 16 ? "After output trim + peak guard" : "After level + ducking; before output trim",
                  { 16, getHeight() - 23, 350, 16 }, 10, quiet);
        }
        if (count == 0)
            label(g, "Play your drums to see the waveforms", plot, 14, quiet, false, juce::Justification::centred);
    }

private:
    static void refreshChoice(juce::ComboBox& control, int& observed, int modelValue)
    {
        // ComboBox delivers user changes asynchronously. An unchanged model
        // must not erase the new selection before its onChange callback runs.
        if (observed == modelValue) return;
        observed = modelValue;
        control.setSelectedId(modelValue, juce::dontSendNotification);
    }
    void saveChoice(juce::ComboBox& control, int& observed, const char* key)
    {
        const int selected = control.getSelectedId();
        if (selected == observed) return; // A model refresh can supersede a queued callback.
        observed = selected;
        owner.parameters.state.setProperty(key, selected, nullptr);
        repaint();
    }
    static void label(juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r,
                      float size, juce::Colour c, bool bold = false,
                      juce::Justification align = juce::Justification::centredLeft)
    {
        g.setColour(c); g.setFont(juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)));
        g.drawText(s, r, align, true);
    }
    juce::String laneCaption(int i) const
    { return juce::String(i + 1).paddedLeft('0', 2) + "  " + owner.laneName(i); }
    const pocket::ScopeFrame& newest() const { return history[(write + history.size() - 1) % history.size()]; }
    const pocket::ScopeFrame& frameAt(size_t i) const { return history[(write + history.size() - count + i) % history.size()]; }

    void drawWave(juce::Graphics& g, juce::Rectangle<int> r, int trace, juce::Colour colour,
                  double start, float seconds, float opacity)
    {
        const int width = r.getWidth();
        if (width <= 0 || r.getHeight() <= 0) return;
        std::vector<pocket::ScopeRange> bins(size_t(width), pocket::ScopeRange {});
        std::vector<bool> present(size_t(width), false);
        for (size_t i = 0; i < count; ++i)
        {
            const auto& f = frameAt(i);
            const double position = (f.seconds - start) / seconds;
            if (position < 0 || position > 1) continue;
            const auto x = size_t(juce::jlimit(0, width - 1, int(position * (width - 1))));
            if (!present[x]) bins[x] = f.waves[size_t(trace)];
            else
            {
                bins[x].min = std::min(bins[x].min, f.waves[size_t(trace)].min);
                bins[x].max = std::max(bins[x].max, f.waves[size_t(trace)].max);
            }
            present[x] = true;
        }
        g.setColour(juce::Colour(0xff29372f)); g.drawHorizontalLine(r.getCentreY(), float(r.getX()), float(r.getRight()));
        juce::Graphics::ScopedSaveState saved(g); g.reduceClipRegion(r);
        const float middle = float(r.getCentreY()), scale = float(r.getHeight()) * 0.46f * float(zoom.getSelectedId());
        g.setColour(colour.withAlpha(opacity));
        for (int x = 0; x < width; ++x)
        {
            if (!present[size_t(x)]) continue;
            const float top = middle - bins[size_t(x)].max * scale;
            const float bottom = middle - bins[size_t(x)].min * scale;
            const float bucketWidth = std::max(1.0f, float(width) / (seconds * float(pocket::scopeFramesPerSecond)));
            g.fillRect(float(r.getX() + x), top, bucketWidth, std::max(1.0f, bottom - top));
        }
    }
    void drawDuck(juce::Graphics& g, juce::Rectangle<int> r, double start, float seconds, juce::Colour colour)
    {
        g.setColour(juce::Colour(0xff29372f)); g.drawHorizontalLine(r.getY() + 1, float(r.getX()), float(r.getRight()));
        juce::Path path; bool begun = false; double last = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const auto& f = frameAt(i); const double position = (f.seconds - start) / seconds;
            if (position < 0 || position > 1) continue;
            const float x = float(r.getX()) + float(position) * float(r.getWidth() - 1);
            const float y = float(r.getY() + 1) + juce::jlimit(0.0f, 1.0f, f.duckDb / 12.0f) * float(r.getHeight() - 3);
            if (!begun || f.seconds - last > 0.02) path.startNewSubPath(x, y); else path.lineTo(x, y);
            begun = true; last = f.seconds;
        }
        g.setColour(colour); g.strokePath(path, juce::PathStrokeType(1.6f));
    }
    PocketBalanceProcessor& owner;
    juce::ComboBox mode, time, zoom;
    int observedMode = 1, observedTime = 2, observedZoom = 2;
    juce::TextButton freeze { "Freeze" };
    std::vector<pocket::ScopeFrame> history;
    size_t write = 0, count = 0;
    double lastReceived = -1000;
};

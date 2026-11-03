#include "StatusBar.h"

#include "OspLookAndFeel.h"

#include <cmath>

namespace osp::ui {

namespace {
juce::String noteName(int midiNote)
{
    static constexpr const char* kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(kNames[((midiNote % 12) + 12) % 12]) + juce::String(midiNote / 12 - 1);
}
} // namespace

StatusBar::StatusBar(osp::app::PluginProcessor& processor) : processor_(processor)
{
    refresh();
    startTimerHz(kTimerHz);
}

StatusBar::~StatusBar()
{
    stopTimer();
}

void StatusBar::updateMeter(float peak, float& db) const
{
    // Instant attack, a fixed fall per tick.
    const float level = peak > 1.0e-5f ? juce::Decibels::gainToDecibels(peak) : -100.0f;
    db = juce::jmax(level, db - kFallDbPerTick);
}

void StatusBar::updateTuner()
{
    const auto& tap = processor_.engine().graph().tuner();
    tunerValid_ = false;
    tunerText_ = "Tuner: --";
    if (!tap.isPrepared())
        return;
    if (detectorRate_ != tap.decimatedRate()) { // prepare() allocates: only when the rate changes
        detectorRate_ = tap.decimatedRate();
        detector_.prepare(detectorRate_);
        window_.assign(static_cast<std::size_t>(detector_.requiredSamples()), 0.0f);
    }
    if (!tap.readLatest(window_.data(), static_cast<int>(window_.size())))
        return; // not enough audio yet, or overwritten while copying: the next poll retries
    const auto reading = detector_.detect(window_.data(), static_cast<int>(window_.size()));
    if (!reading.valid)
        return;
    tunerValid_ = true;
    tunerCents_ = static_cast<float>(reading.cents);
    tunerText_ = "Tuner: " + noteName(reading.midiNote) + "  " + juce::String(juce::roundToInt(reading.cents)) +
                 " ct  " + juce::String(reading.hz, 1) + " Hz";
}

void StatusBar::refresh()
{
    updateMeter(processor_.engine().takeInputPeak(), inputDb_);
    updateMeter(processor_.engine().takeOutputPeak(), outputDb_);
    if (tick_++ % kTunerEvery == 0)
        updateTuner();

    const int latency = processor_.getLatencySamples();
    const double rate = processor_.getSampleRate();
    latencyText_ = "Latency: " + juce::String(latency) + " samples";
    if (rate > 0.0)
        latencyText_ += " (" + juce::String(1000.0 * latency / rate, 1) + " ms)";
    repaint();
}

void StatusBar::drawMeter(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& name, float db) const
{
    g.setColour(colours::dimText);
    g.setFont(juce::Font(juce::FontOptions(area.getHeight() * 0.7f)));
    g.drawText(name, area.removeFromLeft(area.getHeight() * 2.5f), juce::Justification::centredLeft);
    g.setColour(colours::panel);
    g.fillRoundedRectangle(area, 3.0f);
    const float fill = juce::jlimit(0.0f, 1.0f, (db - kFloorDb) / -kFloorDb);
    g.setColour(db > -1.0f ? juce::Colours::red : colours::accent);
    g.fillRoundedRectangle(area.withWidth(area.getWidth() * fill), 3.0f);
}

void StatusBar::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced(8.0f, 4.0f);
    const float row = area.getHeight() / 2.0f;
    auto meters = area.removeFromLeft(area.getWidth() * 0.4f);
    drawMeter(g, meters.removeFromTop(row).reduced(0.0f, 2.0f), "In", inputDb_);
    drawMeter(g, meters.reduced(0.0f, 2.0f), "Out", outputDb_);

    area.removeFromLeft(12.0f);
    auto tuner = area.removeFromLeft(area.getWidth() * 0.62f);
    g.setColour(tunerValid_ ? colours::text : colours::dimText);
    g.setFont(juce::Font(juce::FontOptions(row * 0.9f)));
    g.drawText(tunerText_, tuner.removeFromTop(row), juce::Justification::centredLeft);
    if (tunerValid_) { // a needle on a -50...+50 cent scale
        const auto scale = tuner.reduced(0.0f, 3.0f);
        g.setColour(colours::outline);
        g.fillRect(scale.withSizeKeepingCentre(scale.getWidth(), 2.0f));
        g.setColour(std::abs(tunerCents_) < 5.0f ? colours::accent : colours::text);
        g.fillRect(
            juce::Rectangle<float>(3.0f, scale.getHeight())
                .withCentre({scale.getCentreX() + tunerCents_ / 50.0f * scale.getWidth() / 2.0f, scale.getCentreY()}));
    }

    g.setColour(colours::dimText);
    g.setFont(juce::Font(juce::FontOptions(row * 0.8f)));
    g.drawText(latencyText_, area, juce::Justification::centredRight);
}

} // namespace osp::ui

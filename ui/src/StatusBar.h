#pragma once

#include "PluginProcessor.h"

#include "osp/dsp/tuner/PitchDetector.h"

#include <vector>

namespace osp::ui {

// The bottom strip: input and output meters, the tuner readout and the latency the plugin reports to the host.
// Everything is read on the message thread from lock-free engine values (the peaks and the tuner tap's ring), never
// computed on the audio thread by the editor.
class StatusBar final : public juce::Component, private juce::Timer {
public:
    explicit StatusBar(osp::app::PluginProcessor& processor);
    ~StatusBar() override;

    // One poll of the engine. The timer calls it; tests call it directly.
    void refresh();
    juce::String tunerText() const { return tunerText_; }
    juce::String latencyText() const { return latencyText_; }
    // Displayed meter levels in dBFS (-100 when silent).
    float inputDb() const noexcept { return inputDb_; }
    float outputDb() const noexcept { return outputDb_; }

    void paint(juce::Graphics&) override;

private:
    void timerCallback() override { refresh(); }
    void updateMeter(float peak, float& db) const;
    void updateTuner();
    void drawMeter(juce::Graphics&, juce::Rectangle<float> area, const juce::String& name, float db) const;

    static constexpr float kFloorDb = -60.0f, kFallDbPerTick = 2.0f;
    static constexpr int kTimerHz = 30, kTunerEvery = 4;

    osp::app::PluginProcessor& processor_;
    float inputDb_ = -100.0f, outputDb_ = -100.0f;
    int tick_ = 0;
    osp::dsp::PitchDetector detector_;
    double detectorRate_ = 0.0;
    std::vector<float> window_;
    juce::String tunerText_ = "Tuner: --";
    float tunerCents_ = 0.0f;
    bool tunerValid_ = false;
    juce::String latencyText_;
};

} // namespace osp::ui

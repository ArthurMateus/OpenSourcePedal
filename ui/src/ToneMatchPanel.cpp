#include "ToneMatchPanel.h"

#include "OspLookAndFeel.h"
#include "PresetStore.h"
#include "YtDlp.h"

#include "osp/engine/Preset.h"

#include <atomic>

namespace osp::ui {

namespace fs = std::filesystem;

namespace {

// "83", "83.5" or "1:23" in seconds; empty is "not set" (-1); anything else is invalid (-2).
double parseSeconds(const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty())
        return -1.0;
    if (!t.containsOnly("0123456789.:"))
        return -2.0;
    if (t.contains(":")) {
        const auto minutes = t.upToFirstOccurrenceOf(":", false, false);
        const auto seconds = t.fromFirstOccurrenceOf(":", false, false);
        if (minutes.isEmpty() || seconds.contains(":"))
            return -2.0;
        return minutes.getIntValue() * 60.0 + seconds.getDoubleValue();
    }
    return t.getDoubleValue();
}

bool isLink(const juce::String& s)
{
    return s.startsWithIgnoreCase("http://") || s.startsWithIgnoreCase("https://");
}

} // namespace

// Runs yt-dlp for a link on its own thread: checks that yt-dlp and ffmpeg start, then downloads the audio as WAV.
class ToneMatchPanel::Fetch final : public juce::Thread {
public:
    Fetch(std::string url, fs::path dir) : Thread("yt-dlp fetch"), url_(std::move(url)), dir_(std::move(dir)) {}
    ~Fetch() override { stopThread(60000); }

    bool done() const noexcept { return done_.load(); }
    bool missingTools() const noexcept { return missingTools_.load(); }
    fs::path file() const { return osp::app::ytdlp::outputFile(dir_, url_); }

    void run() override
    {
        const auto works = [](const char* program, const char* flag) {
            juce::ChildProcess probe;
            return probe.start(juce::StringArray{program, flag}) && probe.waitForProcessToFinish(10000) &&
                   probe.getExitCode() == 0;
        };
        if (!works("yt-dlp", "--version") || !works("ffmpeg", "-version")) {
            missingTools_ = true;
        } else {
            std::error_code ec;
            fs::create_directories(dir_, ec);
            if (!fs::exists(file(), ec)) { // the same link is fetched once
                juce::StringArray command{"yt-dlp"};
                for (const auto& a : osp::app::ytdlp::buildArgs(dir_, url_))
                    command.add(juce::String::fromUTF8(a.c_str()));
                juce::ChildProcess child;
                if (child.start(command)) {
                    while (child.isRunning() && !threadShouldExit())
                        wait(100);
                    if (child.isRunning())
                        child.kill();
                }
            }
        }
        done_ = true;
    }

private:
    std::string url_;
    fs::path dir_;
    std::atomic<bool> done_{false}, missingTools_{false};
};

ToneMatchPanel::ToneMatchPanel(osp::app::PluginProcessor& processor, fs::path userDir, fs::path workDir)
    : processor_(processor),
      userDir_(std::move(userDir)),
      workDir_(std::move(workDir))
{
    title_.setText("Tone match", juce::dontSendNotification);
    title_.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    legal_.setText("Only use recordings you have the right to download or copy. Your DI must be your own dry guitar.",
                   juce::dontSendNotification);
    legal_.setColour(juce::Label::textColourId, colours::dimText);
    legal_.setMinimumHorizontalScale(1.0f);
    referenceLabel_.setText("Song (file or link)", juce::dontSendNotification);
    diLabel_.setText("Your DI (WAV)", juce::dontSendNotification);
    sectionLabel_.setText("Part to match (start / end, s or m:ss)", juce::dontSendNotification);
    nameLabel_.setText("Preset name", juce::dontSendNotification);
    name_.setText("Tone match");
    guide_.setButtonText("How to record a DI");
    guide_.setURL(juce::URL("https://github.com/ArthurMateus/OpenSourcePedal/blob/main/docs/guides/recording-a-di.md"));
    warnings_.setMinimumHorizontalScale(1.0f);
    warnings_.setColour(juce::Label::textColourId, colours::accent);

    chooseReference_.onClick = [this] { choose(reference_, "Choose the song"); };
    chooseDi_.onClick = [this] { choose(di_, "Choose your DI"); };
    run_.onClick = [this] { run(); };
    cancel_.onClick = [this] { cancel(); };
    audition_.onClick = [this] { audition(); };
    save_.onClick = [this] { saveResult(); };

    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &title_,    &legal_,       &referenceLabel_, &reference_,    &chooseReference_,
             &diLabel_,  &di_,          &chooseDi_,       &sectionLabel_, &start_,
             &end_,      &stem_,        &nameLabel_,      &name_,         &run_,
             &cancel_,   &progressBar_, &status_,         &audition_,     &save_,
             &warnings_, &guide_})
        addAndMakeVisible(c);
    updateButtons();
    startTimerHz(10);
}

ToneMatchPanel::~ToneMatchPanel()
{
    stopTimer();
    job_.cancel();
    job_.wait();
}

void ToneMatchPanel::setReference(const juce::String& fileOrLink)
{
    reference_.setText(fileOrLink);
}
void ToneMatchPanel::setDi(const juce::String& file)
{
    di_.setText(file);
}
void ToneMatchPanel::setPresetName(const juce::String& name)
{
    name_.setText(name);
}
void ToneMatchPanel::setSection(const juce::String& start, const juce::String& end)
{
    start_.setText(start);
    end_.setText(end);
}

bool ToneMatchPanel::run()
{
    if (phase_ == Phase::fetching || phase_ == Phase::matching) {
        setStatus("A run is already going");
        return false;
    }
    const auto ref = reference_.getText().trim();
    if (ref.isEmpty() || di_.getText().trim().isEmpty()) {
        setStatus("Choose the song and your DI first");
        return false;
    }
    const double s = parseSeconds(start_.getText()), e = parseSeconds(end_.getText());
    if (s == -2.0 || e == -2.0 || (s >= 0.0 && e > 0.0 && e <= s)) {
        setStatus("The part to match must be start < end, in seconds or m:ss");
        return false;
    }
    warnings_.setText({}, juce::dontSendNotification);
    presetJson_ = {};
    progressValue_ = 0.0;
    if (isLink(ref)) {
        if (!osp::app::ytdlp::isAcceptableUrl(ref.toStdString())) {
            setStatus("That link is not a plain http(s) address");
            return false;
        }
        fetch_ = std::make_unique<Fetch>(ref.toStdString(), workDir_);
        fetch_->startThread();
        phase_ = Phase::fetching;
        setStatus("Downloading the audio...");
    } else {
        startMatch(fs::path(ref.toWideCharPointer()));
    }
    updateButtons();
    return phase_ != Phase::idle;
}

void ToneMatchPanel::startMatch(const fs::path& reference)
{
    osp::app::ToneMatchSettings s;
    s.reference = reference;
    s.di = fs::path(di_.getText().trim().toWideCharPointer());
    s.assetDir = processor_.assetDir();
    s.presetName = name_.getText().trim().isEmpty() ? "Tone match" : name_.getText().trim().toStdString();
    const double start = parseSeconds(start_.getText()), end = parseSeconds(end_.getText());
    s.startSeconds = start > 0.0 ? start : 0.0;
    s.endSeconds = end > 0.0 ? end : -1.0;
    s.hpss = !stem_.getToggleState();
    if (job_.start(std::move(s))) {
        phase_ = Phase::matching;
        setStatus("Matching...");
    } else {
        phase_ = Phase::failed;
        setStatus("Could not start the match");
    }
}

void ToneMatchPanel::cancel()
{
    if (phase_ == Phase::fetching) {
        fetch_.reset(); // stops the download
        phase_ = Phase::idle;
        setStatus("Cancelled");
    } else if (phase_ == Phase::matching) {
        job_.cancel();
        setStatus("Cancelling...");
    }
    updateButtons();
}

void ToneMatchPanel::poll()
{
    if (phase_ == Phase::fetching && fetch_ != nullptr && fetch_->done()) {
        const bool missing = fetch_->missingTools();
        const auto file = fetch_->file();
        fetch_.reset();
        std::error_code ec;
        if (missing) {
            phase_ = Phase::failed;
            setStatus(juce::String::fromUTF8(osp::app::ytdlp::installHint().c_str()));
        } else if (!fs::exists(file, ec)) {
            phase_ = Phase::failed;
            setStatus("The download failed (is the link public and ffmpeg installed?)");
        } else {
            startMatch(file);
        }
    } else if (phase_ == Phase::matching) {
        progressValue_ = job_.progress();
        const auto state = job_.state();
        if (state == osp::app::ToneMatchJob::State::running) {
            setStatus(juce::String("Matching: ") + juce::String(job_.stage()));
        } else {
            job_.wait();
            const auto outcome = job_.outcome();
            juce::StringArray warnings;
            for (const auto& w : outcome.warnings)
                warnings.add(juce::String::fromUTF8(w.c_str()));
            warnings_.setText(warnings.joinIntoString("\n"), juce::dontSendNotification);
            if (state == osp::app::ToneMatchJob::State::finished) {
                phase_ = Phase::done;
                presetJson_ = juce::String::fromUTF8(outcome.presetJson.c_str());
                progressValue_ = 1.0;
                setStatus("Done in " + juce::String(outcome.seconds, 0) + " s. Audition it, then save it.");
            } else if (state == osp::app::ToneMatchJob::State::cancelled) {
                phase_ = Phase::idle;
                setStatus("Cancelled");
            } else {
                phase_ = Phase::failed;
                setStatus("Failed: " + juce::String::fromUTF8(outcome.error.c_str()));
            }
        }
    }
    updateButtons();
}

bool ToneMatchPanel::audition()
{
    if (presetJson_.isEmpty())
        return false;
    const auto preset = osp::engine::parsePreset(presetJson_.toStdString());
    if (!preset) {
        setStatus("The result could not be read back");
        return false;
    }
    const auto warnings = processor_.loadPreset(*preset);
    setStatus(warnings.empty() ? "Playing the matched tone"
                               : "Playing it, with " + juce::String(warnings.size()) +
                                     " warning(s): " + juce::String::fromUTF8(warnings.front().c_str()));
    return true;
}

bool ToneMatchPanel::saveResult()
{
    if (presetJson_.isEmpty())
        return false;
    const auto preset = osp::engine::parsePreset(presetJson_.toStdString());
    std::string error;
    if (!preset || !osp::app::saveUserPreset(userDir_, *preset, &error)) {
        setStatus("Cannot save: " + juce::String::fromUTF8(error.c_str()));
        return false;
    }
    setStatus("Saved " + juce::String::fromUTF8(preset->name.c_str()) + " to your presets");
    return true;
}

void ToneMatchPanel::choose(juce::TextEditor& target, const juce::String& title)
{
    chooser_ = std::make_unique<juce::FileChooser>(title, juce::File{}, "*.wav;*.mp3;*.flac;*.ogg;*.aif;*.aiff");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [&target](const juce::FileChooser& fc) {
                              if (fc.getResult() != juce::File{})
                                  target.setText(fc.getResult().getFullPathName());
                          });
}

void ToneMatchPanel::updateButtons()
{
    const bool busy = phase_ == Phase::fetching || phase_ == Phase::matching;
    run_.setEnabled(!busy);
    cancel_.setEnabled(busy);
    audition_.setEnabled(presetJson_.isNotEmpty());
    save_.setEnabled(presetJson_.isNotEmpty());
}

void ToneMatchPanel::resized()
{
    auto area = getLocalBounds().reduced(8);
    const auto row = [&area](int h) { return area.removeFromTop(h); };
    title_.setBounds(row(24));
    legal_.setBounds(row(48));
    referenceLabel_.setBounds(row(20));
    auto r = row(26);
    chooseReference_.setBounds(r.removeFromRight(34));
    reference_.setBounds(r.reduced(0, 1));
    diLabel_.setBounds(row(20));
    r = row(26);
    chooseDi_.setBounds(r.removeFromRight(34));
    di_.setBounds(r.reduced(0, 1));
    sectionLabel_.setBounds(row(20));
    r = row(26);
    start_.setBounds(r.removeFromLeft(r.getWidth() / 2 - 2).reduced(0, 1));
    end_.setBounds(r.withTrimmedLeft(4).reduced(0, 1));
    stem_.setBounds(row(24));
    nameLabel_.setBounds(row(20));
    name_.setBounds(row(26).reduced(0, 1));
    area.removeFromTop(6);
    r = row(28);
    run_.setBounds(r.removeFromLeft(r.getWidth() / 2 - 2));
    cancel_.setBounds(r.withTrimmedLeft(4));
    area.removeFromTop(4);
    progressBar_.setBounds(row(20));
    status_.setBounds(row(40));
    r = row(28);
    audition_.setBounds(r.removeFromLeft(r.getWidth() / 2 - 2));
    save_.setBounds(r.withTrimmedLeft(4));
    warnings_.setBounds(row(60));
    guide_.setBounds(row(22));
}

} // namespace osp::ui

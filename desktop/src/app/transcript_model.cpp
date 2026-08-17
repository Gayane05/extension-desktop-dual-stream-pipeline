// desktop/src/app/transcript_model.cpp
#include "app/transcript_model.h"

#include <algorithm>
#include <cstdio>

namespace dsp
{

// Two-tier state per stream: at most one "pending" (interim) utterance,
// which is wholesale replaced by each new interim from the engine (there is
// only ever one in-flight utterance per lane -- sherpa/deepgram don't emit
// overlapping interims), plus a growing list of "final" utterances that are
// never edited once inserted. isFinal=false events therefore just overwrite
// pending_[stream]; isFinal=true events clear pending_ and graduate the text
// into finals_, keeping finals_ sorted by capture timestamp (not arrival
// order) via upper_bound insertion, since mic and tab finalize independently
// and network jitter can deliver them out of chronological order.
void TranscriptModel::apply(const TranscriptEvent& ev)
{
    std::lock_guard lk(mu_);
    // Interims count toward the baseline too: the session's clock starts at
    // the first words, not at the first completed sentence.
    if (firstTsMs_ < 0.0 || ev.tsMs < firstTsMs_)
    {
        firstTsMs_ = ev.tsMs;
    }
    auto& pending = pending_[static_cast<int>(ev.stream)];
    if (!ev.isFinal)
    {
        pending = Utterance{ev.stream, ev.text, false, ev.tsMs};
        return;
    }
    pending.reset();
    if (ev.text.empty())
    {
        return;
    }
    Utterance utterance{ev.stream, ev.text, true, ev.tsMs};
    auto it = std::upper_bound(
        finals_.begin(), finals_.end(), utterance.tsMs,
        [](double targetTs, const Utterance& existing) { return targetTs < existing.tsMs; });
    finals_.insert(it, std::move(utterance));
}

// Finals are already sorted by tsMs (see apply()); each stream's pending
// interim is appended after them unconditionally, so an in-progress
// utterance always renders below all completed ones regardless of its own
// timestamp -- matching how a live transcript view is expected to read.
std::vector<Utterance> TranscriptModel::snapshot() const
{
    std::lock_guard lk(mu_);
    std::vector<Utterance> out = finals_;
    for (const auto& pendingUtterance : pending_)
    {
        if (pendingUtterance && !pendingUtterance->text.empty())
        {
            out.push_back(*pendingUtterance);
        }
    }
    return out;
}

void TranscriptModel::clear()
{
    std::lock_guard lk(mu_);
    finals_.clear();
    pending_[0].reset();
    pending_[1].reset();
    firstTsMs_ = -1.0;  // The next event starts a fresh session clock.
}

double TranscriptModel::baseTsMs() const
{
    std::lock_guard lk(mu_);
    return firstTsMs_ < 0.0 ? 0.0 : firstTsMs_;
}

std::string formatRelativeTimestamp(double tsMs, double baseTsMs)
{
    const double deltaMs = tsMs - baseTsMs;
    const auto totalMs = deltaMs > 0.0 ? static_cast<long long>(deltaMs) : 0;
    const long long totalSec = totalMs / 1000;
    const long long millis = totalMs % 1000;
    const long long hours = totalSec / 3600;
    char buffer[32];
    if (hours > 0)
    {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld.%03lld", hours,
                      (totalSec / 60) % 60, totalSec % 60, millis);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld.%03lld", totalSec / 60, totalSec % 60,
                      millis);
    }
    return buffer;
}

namespace
{

// A subtitle cue's timing line uses "HH:MM:SS<sep>mmm"; SubRip separates
// milliseconds with a comma, WebVTT with a period.
std::string formatCueTime(double deltaMs, char millisSeparator)
{
    const auto totalMs = deltaMs > 0.0 ? static_cast<long long>(deltaMs) : 0;
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld%c%03lld", totalMs / 3600000,
                  (totalMs / 60000) % 60, (totalMs / 1000) % 60, millisSeparator, totalMs % 1000);
    return buffer;
}

// How long the last cue (and a cue whose successor starts no later than it
// does, e.g. fully overlapping speech) stays on screen.
constexpr double kFallbackCueDurationMs = 3000.0;

}  // namespace

std::string TranscriptModel::toText() const
{
    std::lock_guard lk(mu_);
    std::string out;
    for (const auto& utterance : finals_)
    {
        out += "[" + formatRelativeTimestamp(utterance.tsMs, firstTsMs_) + "] " +
               (utterance.stream == StreamId::Mic ? "You: " : "Others: ") + utterance.text + "\n";
    }
    return out;
}

// Shared cue emission for both subtitle formats: finals_ is already sorted
// by capture timestamp (see apply()), so cue N runs from its own start to
// cue N+1's start; the last cue -- or one overtaken by an overlapping lane
// -- gets a fixed on-screen duration instead.
std::string TranscriptModel::toSrt() const
{
    std::lock_guard lk(mu_);
    std::string out;
    for (size_t i = 0; i < finals_.size(); ++i)
    {
        const double start = finals_[i].tsMs - firstTsMs_;
        double end = start + kFallbackCueDurationMs;
        if (i + 1 < finals_.size() && finals_[i + 1].tsMs > finals_[i].tsMs)
        {
            end = finals_[i + 1].tsMs - firstTsMs_;
        }
        out += std::to_string(i + 1) + "\n";
        out += formatCueTime(start, ',') + " --> " + formatCueTime(end, ',') + "\n";
        out += std::string(finals_[i].stream == StreamId::Mic ? "[You] " : "[Others] ") +
               finals_[i].text + "\n\n";
    }
    return out;
}

std::string TranscriptModel::toVtt() const
{
    std::lock_guard lk(mu_);
    std::string out = "WEBVTT\n\n";
    for (size_t i = 0; i < finals_.size(); ++i)
    {
        const double start = finals_[i].tsMs - firstTsMs_;
        double end = start + kFallbackCueDurationMs;
        if (i + 1 < finals_.size() && finals_[i + 1].tsMs > finals_[i].tsMs)
        {
            end = finals_[i + 1].tsMs - firstTsMs_;
        }
        out += formatCueTime(start, '.') + " --> " + formatCueTime(end, '.') + "\n";
        out += std::string(finals_[i].stream == StreamId::Mic ? "[You] " : "[Others] ") +
               finals_[i].text + "\n\n";
    }
    return out;
}

}  // namespace dsp

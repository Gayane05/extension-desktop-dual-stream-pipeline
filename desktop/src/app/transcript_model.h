// desktop/src/app/transcript_model.h
//
// Thread-safe accumulator for what the UI shows: STT engines (stt/*) call
// apply() from their own decode/callback threads as TranscriptEvents arrive,
// and the UI thread (main_window.cpp) calls snapshot()/toText() to render or
// save. Owns the interim-vs-final merge logic so engines only need to report
// events as they happen, without worrying about display ordering.
#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/protocol.h"

namespace dsp
{

struct TranscriptEvent
{
    StreamId stream;
    std::string text;
    bool isFinal;
    double tsMs;
};
struct Utterance
{
    StreamId stream;
    std::string text;
    bool isFinal;
    double tsMs;
};

// Formats a capture timestamp relative to a session baseline as
// "mm:ss.mmm", growing to "h:mm:ss.mmm" past an hour ("03:12.480",
// "1:02:03.450"). All user-facing
// timestamps are meeting-relative -- counted from the session's first words
// -- because capture clocks differ by audio source (the extension sends epoch
// time, test feeders send a monotonic clock) and only deltas are meaningful
// across all of them. A timestamp earlier than the baseline clamps to 00:00.
std::string formatRelativeTimestamp(double tsMs, double baseTsMs);

class TranscriptModel
{
public:
    void apply(const TranscriptEvent& ev);
    std::vector<Utterance> snapshot() const;
    void clear();
    // Capture timestamp of the session's earliest event (interims included),
    // or 0.0 before any event arrives; the baseline every displayed
    // timestamp is measured from. clear() starts a fresh session.
    double baseTsMs() const;
    // Plain-text transcript: "[03:12] You: ..." lines, finals only.
    std::string toText() const;
    // SubRip subtitles (finals only, sorted by start). Each cue runs until
    // the next cue begins -- overlapping speech across lanes yields
    // overlapping cues, which players render simultaneously.
    std::string toSrt() const;
    // WebVTT subtitles: the same cues as toSrt() in WebVTT syntax.
    std::string toVtt() const;

private:
    mutable std::mutex mu_;
    std::vector<Utterance> finals_;
    std::optional<Utterance> pending_[kStreamCount];
    double firstTsMs_ = -1.0;  // Negative until the first event arrives.
};

}  // namespace dsp

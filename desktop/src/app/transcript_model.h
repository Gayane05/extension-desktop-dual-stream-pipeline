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

class TranscriptModel
{
public:
    void apply(const TranscriptEvent& ev);
    std::vector<Utterance> snapshot() const;
    void clear();
    std::string toText() const;

private:
    mutable std::mutex mu_;
    std::vector<Utterance> finals_;
    std::optional<Utterance> pending_[kStreamCount];
};

}  // namespace dsp

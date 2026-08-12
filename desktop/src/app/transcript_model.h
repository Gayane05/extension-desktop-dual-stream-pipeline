#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "core/protocol.h"

namespace dsp {

struct TranscriptEvent { StreamId stream; std::string text; bool isFinal; double tsMs; };
struct Utterance { StreamId stream; std::string text; bool isFinal; double tsMs; };

class TranscriptModel {
public:
    void apply(const TranscriptEvent& ev);
    std::vector<Utterance> snapshot() const;
    void clear();
    std::string toText() const;

private:
    mutable std::mutex mu_;
    std::vector<Utterance> finals_;
    std::optional<Utterance> pending_[2];
};

}  // namespace dsp

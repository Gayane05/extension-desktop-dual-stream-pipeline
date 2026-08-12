// desktop/src/stt/deepgram_engine.h
#pragma once
#include <memory>
#include <optional>
#include <string>
#include "stt/stt_engine.h"

namespace ix { class WebSocket; }

namespace dsp {

std::optional<TranscriptEvent> parseDeepgramMessage(StreamId s, const std::string& json,
                                                    double nowMs);

// Threading: start() must complete before any feed() calls begin (feeder
// threads are spawned only after start() returns), mirroring SherpaEngine's
// contract. feed() is safe to call from multiple threads concurrently -- each
// stream owns its own ix::WebSocket connection (ws_[0] for Mic, ws_[1] for
// Tab), so the two feeder threads never touch shared mutable state. stop()
// must not run concurrently with feed() -- callers (Pipeline) join feeder
// threads before stopping the engine. The destructor calls stop(), so the
// same rule applies to destruction. ix::WebSocket delivers onMessage
// callbacks from its own internal background thread, not from feed()'s
// caller thread; those callbacks only invoke cb_ (assumed callback-safe, per
// SherpaEngine's contract) and never touch engine state, so no additional
// locking is needed here.
class DeepgramEngine : public ISttEngine {
public:
    DeepgramEngine(EngineOptions opts, TranscriptCallback cb);
    ~DeepgramEngine() override;
    bool start(std::string& error) override;
    void feed(StreamId s, const int16_t* samples, size_t n, double tsMs) override;
    void stop() override;
    std::string name() const override { return "deepgram"; }
    std::string effectiveProvider() const override { return "cloud"; }

private:
    EngineOptions opts_;
    TranscriptCallback cb_;
    std::unique_ptr<ix::WebSocket> ws_[2];  // one live connection per stream
};

}  // namespace dsp

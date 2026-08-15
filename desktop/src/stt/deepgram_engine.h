// desktop/src/stt/deepgram_engine.h
#pragma once
#include <memory>
#include <optional>
#include <string>

#include "stt/stt_engine.h"

namespace ix
{
class WebSocket;
}

namespace dsp
{

// Parses one Deepgram streaming "Results" message into a TranscriptEvent
// (or nullopt for message types/shapes we don't care about). The event's
// tsMs is the UTTERANCE START time -- connectionEpochMs plus the result's
// "start" offset (seconds into the audio stream) -- so that overlapping
// speech across lanes sorts in the order people began talking, not the
// order segments happened to finalize. Free function (not a method) so it's
// unit-testable without spinning up a real WS connection -- see
// desktop/tests/test_deepgram_parse.cpp.
std::optional<TranscriptEvent> parseDeepgramMessage(StreamId streamId, const std::string& json,
                                                    double connectionEpochMs);

// Cloud STT via Deepgram's streaming API. Implements ISttEngine; unlike
// SherpaEngine there is no local model or shared decode state -- each stream
// gets its own independent WS connection to Deepgram (ws_[0]/ws_[1]), so
// this class is mostly a thin PCM-forwarder plus message-to-event parsing.
//
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
class DeepgramEngine : public ISttEngine
{
public:
    DeepgramEngine(EngineOptions opts, TranscriptCallback cb);
    ~DeepgramEngine() override;
    bool start(std::string& error) override;
    void feed(StreamId streamId, const int16_t* samples, size_t sampleCount, double tsMs) override;
    void stop() override;
    std::string name() const override { return "deepgram"; }
    std::string effectiveProvider() const override { return "cloud"; }

private:
    EngineOptions opts_;
    TranscriptCallback cb_;
    std::unique_ptr<ix::WebSocket> ws_[kStreamCount];  // One live connection per stream.
    // Wall-clock ms when each connection opened; Deepgram result "start"
    // offsets are relative to this. Written on Open and read on Message,
    // both delivered by the same per-connection ixwebsocket thread, so no
    // synchronization is needed. Reset on every (re)connect because the
    // stream's timeline restarts at zero.
    double connectionEpochMs_[kStreamCount] = {};
};

}  // namespace dsp

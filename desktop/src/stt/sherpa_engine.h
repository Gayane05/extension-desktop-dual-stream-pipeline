// desktop/src/stt/sherpa_engine.h
#pragma once
#include <mutex>
#include <string>

#include "stt/stt_engine.h"

typedef struct SherpaOnnxOnlineRecognizer SherpaOnnxOnlineRecognizer;
typedef struct SherpaOnnxOnlineStream SherpaOnnxOnlineStream;

namespace dsp
{

// Local, on-device STT via sherpa-onnx's streaming zipformer transducer.
// Implements ISttEngine; owns one recognizer shared by both streams (Mic and
// Tab each get their own SherpaOnnxOnlineStream, decoded under a shared
// mutex) so model weights are loaded once regardless of how many lanes are
// active. See sherpa_engine.cpp for endpointing/decoding-method rationale.
//
// Threading: start() must complete before any feed() calls begin (feeder
// threads are spawned only after start() returns). feed() is safe to call
// from multiple threads concurrently (the internal mutex serializes decode
// across them). stop() must not run concurrently with feed() -- callers
// (Pipeline) join feeder threads before stopping the engine. The destructor
// calls stop(), so the same rule applies to destruction. start() itself also
// takes the mutex (see sherpa_engine.cpp), so the class is self-defending
// even if that lifecycle contract is ever violated.
class SherpaEngine : public ISttEngine
{
public:
    SherpaEngine(EngineOptions opts, TranscriptCallback cb);
    ~SherpaEngine() override;
    bool start(std::string& error) override;
    void feed(StreamId streamId, const int16_t* samples, size_t sampleCount, double tsMs) override;
    void stop() override;
    std::string name() const override { return "sherpa"; }
    std::string effectiveProvider() const override { return effectiveProvider_; }

private:
    // Private helper invoked only from start(), which already holds mu_;
    // does not lock itself (would self-deadlock on a non-recursive mutex).
    bool createRecognizer(const std::string& provider, std::string& error);

    EngineOptions opts_;
    TranscriptCallback cb_;
    std::string effectiveProvider_;
    const SherpaOnnxOnlineRecognizer* rec_ = nullptr;
    const SherpaOnnxOnlineStream* streams_[kStreamCount] = {nullptr, nullptr};
    std::string lastInterim_[kStreamCount];
    // True once the current utterance (since the last endpoint reset) has seen
    // audio above the digital-silence floor. Gates emitted text: beam search
    // hallucinates tokens on pure zeros (muted mic/tab), verified empirically.
    bool voiced_[kStreamCount] = {false, false};
    std::mutex mu_;  // Serializes decode across the two feeder threads.
};

}  // namespace dsp

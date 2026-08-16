// desktop/src/stt/parakeet_engine.h
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include "stt/stt_engine.h"

typedef struct SherpaOnnxOfflineRecognizer SherpaOnnxOfflineRecognizer;
typedef struct SherpaOnnxVoiceActivityDetector SherpaOnnxVoiceActivityDetector;

namespace dsp
{

// Maps a VAD sample index (offset from the first sample the stream ever fed)
// to a capture timestamp, given the capture time of that first sample. Used
// to stamp Parakeet finals with the utterance's START so overlapping speech
// across lanes sorts in speaking order, matching the other engines.
inline double sampleIndexToTsMs(double streamStartTsMs, int64_t sampleIndex)
{
    return streamStartTsMs + (static_cast<double>(sampleIndex) * 1000.0) / kSampleRateHz;
}

// Local, on-device STT via NVIDIA's Parakeet TDT 0.6B (a NON-streaming NeMo
// transducer) running on sherpa-onnx's OfflineRecognizer. Because the model
// only transcribes complete segments, a Silero VAD chops each lane's live
// audio into utterances (the pause length reuses --endpoint-silence) and
// every closed segment is decoded whole. The result is near-live: finals
// appear ~1-2 s after each pause, and there are NO word-by-word interims --
// that is the architectural trade for the highest local accuracy.
//
// Threading: start() must complete before any feed() calls begin (feeder
// threads are spawned only after start() returns). Each lane's VAD and
// timestamp state are touched only by that lane's feeder thread; the shared
// offline recognizer is serialized by an internal mutex. stop() must not run
// concurrently with feed() -- callers (Pipeline) join feeder threads before
// stopping the engine; the destructor calls stop(), so the same rule applies
// to destruction. The callback runs while the decode mutex is held and must
// not call back into this engine.
class ParakeetEngine : public ISttEngine
{
public:
    ParakeetEngine(EngineOptions opts, TranscriptCallback cb);
    ~ParakeetEngine() override;
    bool start(std::string& error) override;
    void feed(StreamId streamId, const int16_t* samples, size_t sampleCount, double tsMs) override;
    void stop() override;
    std::string name() const override { return "parakeet"; }
    std::string effectiveProvider() const override { return effectiveProvider_; }

private:
    // Private helper invoked only from start(); builds the offline
    // recognizer for one provider so start() can fall back to cpu.
    bool createRecognizer(const std::string& provider, std::string& error);
    // Decodes one closed VAD segment and emits its final event. Serialized
    // by decodeMu_ (shared recognizer, called from both feeder threads).
    void decodeSegment(StreamId streamId, const float* samples, int32_t sampleCount,
                       int32_t startSampleIndex);
    // Drains every segment the lane's VAD has closed so far.
    void drainSegments(StreamId streamId);

    EngineOptions opts_;
    TranscriptCallback cb_;
    std::string effectiveProvider_;
    const SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;
    const SherpaOnnxVoiceActivityDetector* vads_[kStreamCount] = {nullptr, nullptr};
    // Capture timestamp of the very first sample each lane fed; VAD segment
    // sample offsets are relative to it (see sampleIndexToTsMs). Negative
    // means the lane has not fed audio yet.
    double streamStartTsMs_[kStreamCount] = {-1.0, -1.0};
    std::mutex decodeMu_;  // Serializes offline decodes across feeder threads.
};

}  // namespace dsp

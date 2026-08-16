// desktop/src/stt/parakeet_engine.h
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
// every closed segment is decoded whole for the FINAL event.
//
// Interims are pseudo-streaming: while the VAD reports speech in progress,
// the audio accumulated since the utterance began is re-decoded every
// ~kInterimIntervalSec on a dedicated worker thread and emitted as an
// interim event, so text appears ~1-2 s after speech starts instead of only
// after the pause. Each re-decode sees more context than the last, so the
// interim line refines itself and the final (decoded from the exact
// VAD-trimmed segment) replaces it.
//
// Threading: start() must complete before any feed() calls begin (feeder
// threads are spawned only after start() returns). Each lane's VAD, history
// buffer, and timestamp state are touched only by that lane's feeder thread.
// Final decodes run on the feeder threads; interim decodes run on the single
// interim worker thread; both are serialized on the shared recognizer by
// decodeMu_. stop() must not run concurrently with feed() -- callers
// (Pipeline) join feeder threads before stopping the engine; the destructor
// calls stop(), so the same rule applies to destruction. The callback runs
// while decodeMu_ is held and must not call back into this engine.
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
    // A snapshot of one lane's open-utterance audio, queued for an interim
    // re-decode. Latest-wins per lane: the feeder overwrites its lane's slot
    // if the worker has not picked the previous snapshot up yet.
    struct InterimJob
    {
        bool valid = false;
        std::vector<float> samples;
        double tsMs = 0.0;        // Capture time of the utterance's start.
        uint64_t generation = 0;  // Utterance generation at snapshot time.
    };

    // Private helper invoked only from start(); builds the offline
    // recognizer for one provider so start() can fall back to cpu.
    bool createRecognizer(const std::string& provider, std::string& error);
    // Decodes samples on the shared recognizer and returns the raw text.
    // REQUIRES decodeMu_ to be held by the caller.
    std::string decodeAudioLocked(const float* samples, int32_t sampleCount);
    // Decodes one closed VAD segment and emits its final event.
    void decodeSegment(StreamId streamId, const float* samples, int32_t sampleCount,
                       int32_t startSampleIndex);
    // Drains every segment the lane's VAD has closed so far.
    void drainSegments(StreamId streamId);
    // Tracks the open utterance and queues interim snapshots at the cadence
    // constants define. Runs on the lane's feeder thread.
    void maybeQueueInterim(StreamId streamId);
    // Interim worker thread body: picks up snapshots, re-decodes, emits
    // interim events (unless the utterance was finalized meanwhile).
    void interimWorkerLoop();

    EngineOptions opts_;
    TranscriptCallback cb_;
    std::string effectiveProvider_;
    const SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;
    const SherpaOnnxVoiceActivityDetector* vads_[kStreamCount] = {nullptr, nullptr};
    // Capture timestamp of the very first sample each lane fed; VAD segment
    // sample offsets are relative to it (see sampleIndexToTsMs). Negative
    // means the lane has not fed audio yet.
    double streamStartTsMs_[kStreamCount] = {-1.0, -1.0};
    std::mutex decodeMu_;  // Serializes offline decodes across all threads.

    // --- Per-lane state below is touched only by that lane's feeder thread.
    // Rolling float history of recently fed audio; history_[i][0] is absolute
    // sample index historyBase_[i]. Interim snapshots are cut from here.
    std::vector<float> history_[kStreamCount];
    int64_t historyBase_[kStreamCount] = {0, 0};
    int64_t absSampleCount_[kStreamCount] = {0, 0};  // Total samples fed so far.
    // Absolute start of the utterance currently open per the VAD, or -1 when
    // the lane is idle. End of the last VAD-closed segment, so a new
    // utterance can never reach back into already-finalized audio.
    int64_t openStartAbs_[kStreamCount] = {-1, -1};
    int64_t lastFinalEndAbs_[kStreamCount] = {0, 0};
    // Absolute sample count at which the next interim snapshot is due.
    int64_t nextInterimAtAbs_[kStreamCount] = {0, 0};

    // --- Interim worker plumbing.
    // Bumped by the feeder when a lane's utterance closes; the worker drops
    // snapshots whose generation is stale so a superseded interim can never
    // print after its final.
    std::atomic<uint64_t> utteranceGen_[kStreamCount] = {0, 0};
    std::mutex interimMu_;  // Guards interimJobs_ and interimStop_.
    std::condition_variable interimCv_;
    InterimJob interimJobs_[kStreamCount];
    bool interimStop_ = false;
    std::thread interimWorker_;
    // Worker-thread-only: dedupes repeated identical interim decodes within
    // one utterance (the generation disambiguates across utterances).
    std::string lastInterimText_[kStreamCount];
    uint64_t lastInterimGen_[kStreamCount] = {0, 0};
};

}  // namespace dsp

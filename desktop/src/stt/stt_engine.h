// desktop/src/stt/stt_engine.h
//
// Engine-agnostic interface Pipeline's worker threads feed audio into (see
// pipeline.cpp's workerLoop) and that emits TranscriptEvents into
// TranscriptModel via TranscriptCallback. Two implementations: SherpaEngine
// (local ONNX model) and DeepgramEngine (cloud WS API) -- main.cpp's
// makeEngine() picks one based on Config::engine. Callers must not call
// feed() before start() returns, nor concurrently with stop() (see each
// implementation's header for the precise threading contract).
#pragma once
#include <functional>
#include <string>

#include "app/transcript_model.h"
#include "core/protocol.h"

namespace dsp
{

using TranscriptCallback = std::function<void(const TranscriptEvent&)>;

struct EngineOptions
{
    std::string modelDir;  // Dir containing encoder*.onnx decoder*.onnx joiner*.onnx tokens.txt.
    std::string provider = "cpu";     // cpu|cuda|tensorrt (sherpa).
    std::string decoding = "beam";    // beam|greedy (sherpa).
    double endpointSilenceSec = 0.8;  // sherpa endpoint rule2 (see config.h).
    std::string deepgramKey;          // deepgram only.
    // Deepgram only: BCP-47 code ("en", "es") or "multi" for automatic
    // multilingual transcription with code-switching. Local models are
    // English exports and ignore this.
    std::string language = "multi";
};

class ISttEngine
{
public:
    virtual ~ISttEngine() = default;
    virtual bool start(std::string& error) = 0;  // May downgrade provider (see effectiveProvider).
    virtual void feed(StreamId streamId, const int16_t* samples, size_t sampleCount,
                      double tsMs) = 0;
    virtual void stop() = 0;
    virtual std::string name() const = 0;
    virtual std::string effectiveProvider() const = 0;  // "cpu" after fallback.
    // Health signal for failures that happen AFTER start() succeeded:
    // non-empty while the engine is up but currently unable to deliver
    // results (e.g. Deepgram's async connections rejected with a bad key),
    // empty when healthy. The UI polls this to surface such failures in the
    // status bar. Local engines have no post-start failure mode and keep
    // the default. Must be safe to call from any thread.
    virtual std::string runtimeError() const { return ""; }
};

}  // namespace dsp

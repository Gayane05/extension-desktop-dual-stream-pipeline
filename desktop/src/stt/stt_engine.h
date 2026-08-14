// desktop/src/stt/stt_engine.h
#pragma once
#include <functional>
#include <string>

#include "app/transcript_model.h"
#include "core/protocol.h"

namespace dsp {

using TranscriptCallback = std::function<void(const TranscriptEvent&)>;

struct EngineOptions {
    std::string modelDir;  // dir containing encoder*.onnx decoder*.onnx joiner*.onnx tokens.txt
    std::string provider = "cpu";     // cpu|cuda|tensorrt (sherpa)
    std::string decoding = "beam";    // beam|greedy (sherpa)
    double endpointSilenceSec = 0.8;  // sherpa endpoint rule2 (see config.h)
    std::string deepgramKey;          // deepgram only
};

class ISttEngine {
public:
    virtual ~ISttEngine() = default;
    virtual bool start(std::string& error) = 0;  // may downgrade provider (see effectiveProvider)
    virtual void feed(StreamId s, const int16_t* samples, size_t n, double tsMs) = 0;
    virtual void stop() = 0;
    virtual std::string name() const = 0;
    virtual std::string effectiveProvider() const = 0;  // "cpu" after fallback
};

}  // namespace dsp

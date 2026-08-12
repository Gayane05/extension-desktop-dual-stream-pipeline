// desktop/src/stt/sherpa_engine.h
#pragma once
#include <mutex>
#include <string>

#include "stt/stt_engine.h"

typedef struct SherpaOnnxOnlineRecognizer SherpaOnnxOnlineRecognizer;
typedef struct SherpaOnnxOnlineStream SherpaOnnxOnlineStream;

namespace dsp {

class SherpaEngine : public ISttEngine {
public:
    SherpaEngine(EngineOptions opts, TranscriptCallback cb);
    ~SherpaEngine() override;
    bool start(std::string& error) override;
    void feed(StreamId s, const int16_t* samples, size_t n, double tsMs) override;
    void stop() override;
    std::string name() const override { return "sherpa"; }
    std::string effectiveProvider() const override { return effectiveProvider_; }

private:
    bool createRecognizer(const std::string& provider, std::string& error);

    EngineOptions opts_;
    TranscriptCallback cb_;
    std::string effectiveProvider_;
    const SherpaOnnxOnlineRecognizer* rec_ = nullptr;
    const SherpaOnnxOnlineStream* streams_[2] = {nullptr, nullptr};
    std::string lastInterim_[2];
    std::mutex mu_;  // serializes decode across the two feeder threads
};

}  // namespace dsp

#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include "app/config.h"
#include "app/transcript_model.h"
#include "core/spsc_ring.h"
#include "net/ws_server.h"
#include "stt/stt_engine.h"

namespace dsp {

class Pipeline {
public:
    Pipeline(const Config& cfg, ISttEngine& engine, TranscriptModel& model);
    ~Pipeline();
    bool start(std::string& error);
    void stop();
    uint64_t droppedChunks(StreamId s) const { return dropped_[static_cast<int>(s)].load(); }
    std::string streamState(StreamId s) const {
        return lastFrameCount_[static_cast<int>(s)].load() > 0 ? "streaming" : "idle";
    }
    bool clientConnected() const { return connected_.load(); }

private:
    void workerLoop(StreamId s);
    void pushStatus();

    Config cfg_;
    ISttEngine& engine_;
    TranscriptModel& model_;
    SpscRing<AudioFrame> rings_[2]{SpscRing<AudioFrame>(256), SpscRing<AudioFrame>(256)};
    std::atomic<uint64_t> dropped_[2]{};
    std::atomic<uint64_t> lastFrameCount_[2]{};
    std::atomic<bool> connected_{false};
    std::unique_ptr<WsServer> server_;
    std::thread workers_[2];
};

}  // namespace dsp

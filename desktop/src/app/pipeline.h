#pragma once
#include <atomic>
#include <chrono>
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
    // "streaming" only while a frame arrived within the last 2s, so the UI
    // badge doesn't lie forever once a client disconnects mid-stream.
    std::string streamState(StreamId s) const {
        const int64_t last = lastFrameMs_[static_cast<int>(s)].load();
        if (last == 0) return "idle";
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return (nowMs - last) <= 2000 ? "streaming" : "idle";
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
    std::atomic<int64_t> lastFrameMs_[2]{};
    std::atomic<bool> connected_{false};
    // Set before server_->stop() in Pipeline::stop() so pushStatus() (called
    // from ix connection threads via onHello/onClientGone) stops touching
    // server_ before it is reset from the main thread. See pushStatus().
    std::atomic<bool> stopping_{false};
    std::unique_ptr<WsServer> server_;
    std::thread workers_[2];
};

}  // namespace dsp

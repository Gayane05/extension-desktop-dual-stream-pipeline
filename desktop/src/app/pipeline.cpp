// desktop/src/app/pipeline.cpp
#include "app/pipeline.h"

#include <cstdio>
#include <exception>

#include "core/protocol.h"

namespace dsp
{

Pipeline::Pipeline(const Config& cfg, ISttEngine& engine) : cfg_(cfg), engine_(engine) {}

Pipeline::~Pipeline()
{
    stop();
}

bool Pipeline::start(std::string& error)
{
    server_ = std::make_unique<WsServer>(
        cfg_.port,
        WsServer::Callbacks{
            // Demux: onAudio fires on a WsServer connection thread for both
            // streams; frame.stream picks which of the two per-stream rings this
            // frame lands in, decoupling network delivery from STT decode
            // pacing on the corresponding workerLoop below.
            .onAudio =
                [this](AudioFrame&& frame) {
                    const int streamIndex = static_cast<int>(frame.stream);
                    const StreamId streamId = frame.stream;  // Capture before tryPush moves frame.
                    const auto now = std::chrono::steady_clock::now().time_since_epoch();
                    lastFrameMs_[streamIndex].store(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
                    // tryPush drop policy: if the STT worker is falling behind
                    // (ring full), drop the newest frame rather than blocking
                    // the connection thread or growing the ring unbounded --
                    // audio is real-time data, so a dropped chunk is strictly
                    // better than an ever-growing backlog of stale audio. Log
                    // only the first drop per lane to avoid flooding stderr
                    // during a sustained backlog; droppedChunks() still counts
                    // every one for the UI.
                    if (!rings_[streamIndex].tryPush(std::move(frame)))
                    {
                        if (dropped_[streamIndex]++ == 0)
                        {
                            std::fprintf(stderr, "warning: dropping frames for %s (ring full)\n",
                                         streamName(streamId));
                        }
                    }
                },
            .onHello =
                [this](const HelloInfo&) {
                    connected_ = true;
                    pushStatus();
                },
            // NOTE: onClientGone may fire twice for one disconnect (an explicit
            // "bye" message followed by the socket's Close event). Both handlers
            // below are idempotent: setting an atomic bool to false twice and
            // re-broadcasting the same status JSON twice are both harmless.
            .onClientGone =
                [this] {
                    connected_ = false;
                    pushStatus();
                },
        });
    if (!server_->start(error))
    {
        return false;
    }
    workers_[0] = std::thread([this] { workerLoop(StreamId::Mic); });
    workers_[1] = std::thread([this] { workerLoop(StreamId::Tab); });
    return true;
}

// Consumer side of the demux -> ring -> worker flow: one dedicated thread
// per stream (see start()) drains this stream's ring and feeds the shared
// STT engine, so mic and tab decode independently and neither lane's decode
// latency can stall the other's frame delivery.
void Pipeline::workerLoop(StreamId streamId)
{
    auto& ring = rings_[static_cast<int>(streamId)];
    while (auto frame = ring.popWait())
    {
        // A crash inside the engine must not take the whole process down
        // silently mid-worker-loop; log and stop this lane's worker rather
        // than letting an exception escape a detached-looking thread.
        try
        {
            engine_.feed(streamId, frame->samples.data(), frame->samples.size(),
                         frame->captureTsMs);
            processedSamples_[static_cast<int>(streamId)] += frame->samples.size();
        }
        catch (const std::exception& ex)
        {
            std::fprintf(stderr, "engine feed error (%s): %s\n", streamName(streamId), ex.what());
            return;
        }
        catch (...)
        {
            std::fprintf(stderr, "engine feed error (%s): unknown exception\n",
                         streamName(streamId));
            return;
        }
    }
}

void Pipeline::pushStatus()
{
    // stopping_ is set (by stop(), below) before server_->stop() runs, and
    // pushStatus() is only ever invoked from WsServer's connection-thread
    // callbacks (onHello/onClientGone) -- never from the main thread. Bailing
    // out here once stopping_ is visible means no new broadcast() call can
    // start touching server_ after stop() begins tearing it down; any
    // broadcast() already in flight still completes safely because
    // WsServer::stop() joins its connection threads before stop() resets
    // server_ (ixwebsocket's ws->stop() blocks until the connection thread
    // exits). This narrows the use-after-free window to practical zero
    // without needing a mutex around every broadcast.
    if (stopping_.load())
    {
        return;
    }
    if (server_)
    {
        server_->broadcast(buildStatusJson(engine_.name(), engine_.effectiveProvider(),
                                           streamState(StreamId::Mic), streamState(StreamId::Tab)));
    }
}

void Pipeline::stop()
{
    stopping_.store(true);
    if (server_)
    {
        server_->stop();
    }
    for (auto& ring : rings_)
    {
        ring.close();
    }
    for (auto& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    server_.reset();
}

}  // namespace dsp

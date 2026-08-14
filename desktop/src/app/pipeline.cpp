#include "app/pipeline.h"

#include <cstdio>
#include <exception>

#include "core/protocol.h"

namespace dsp {

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
            .onAudio =
                [this](AudioFrame&& f) {
                    const int i = static_cast<int>(f.stream);
                    const StreamId sid = f.stream;  // capture before tryPush moves f
                    const auto now = std::chrono::steady_clock::now().time_since_epoch();
                    lastFrameMs_[i].store(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
                    if (!rings_[i].tryPush(std::move(f)))
                    {
                        if (dropped_[i]++ == 0)
                        {
                            std::fprintf(stderr, "warning: dropping frames for %s (ring full)\n",
                                         streamName(sid));
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

void Pipeline::workerLoop(StreamId s)
{
    auto& ring = rings_[static_cast<int>(s)];
    while (auto frame = ring.popWait())
    {
        // A crash inside the engine must not take the whole process down
        // silently mid-worker-loop; log and stop this lane's worker rather
        // than letting an exception escape a detached-looking thread.
        try
        {
            engine_.feed(s, frame->samples.data(), frame->samples.size(), frame->captureTsMs);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "engine feed error (%s): %s\n", streamName(s), e.what());
            return;
        }
        catch (...)
        {
            std::fprintf(stderr, "engine feed error (%s): unknown exception\n", streamName(s));
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
    for (auto& r : rings_)
    {
        r.close();
    }
    for (auto& w : workers_)
    {
        if (w.joinable())
        {
            w.join();
        }
    }
    server_.reset();
}

}  // namespace dsp

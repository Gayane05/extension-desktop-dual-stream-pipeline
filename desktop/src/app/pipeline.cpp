#include "app/pipeline.h"

#include "core/protocol.h"

namespace dsp {

Pipeline::Pipeline(const Config& cfg, ISttEngine& engine, TranscriptModel& model)
    : cfg_(cfg), engine_(engine), model_(model) {}

Pipeline::~Pipeline() { stop(); }

bool Pipeline::start(std::string& error) {
    server_ = std::make_unique<WsServer>(cfg_.port, WsServer::Callbacks{
        .onAudio = [this](AudioFrame&& f) {
            const int i = static_cast<int>(f.stream);
            lastFrameCount_[i]++;
            if (!rings_[i].tryPush(std::move(f))) dropped_[i]++;
        },
        .onHello = [this](const HelloInfo&) { connected_ = true; pushStatus(); },
        // NOTE: onClientGone may fire twice for one disconnect (an explicit
        // "bye" message followed by the socket's Close event). Both handlers
        // below are idempotent: setting an atomic bool to false twice and
        // re-broadcasting the same status JSON twice are both harmless.
        .onClientGone = [this] { connected_ = false; pushStatus(); },
    });
    if (!server_->start(error)) return false;
    workers_[0] = std::thread([this] { workerLoop(StreamId::Mic); });
    workers_[1] = std::thread([this] { workerLoop(StreamId::Tab); });
    return true;
}

void Pipeline::workerLoop(StreamId s) {
    auto& ring = rings_[static_cast<int>(s)];
    while (auto frame = ring.popWait()) {
        engine_.feed(s, frame->samples.data(), frame->samples.size(), frame->captureTsMs);
    }
}

void Pipeline::pushStatus() {
    if (server_)
        server_->broadcast(buildStatusJson(engine_.name(), engine_.effectiveProvider(),
                                           streamState(StreamId::Mic),
                                           streamState(StreamId::Tab)));
}

void Pipeline::stop() {
    if (server_) server_->stop();
    for (auto& r : rings_) r.close();
    for (auto& w : workers_) if (w.joinable()) w.join();
    server_.reset();
}

}  // namespace dsp

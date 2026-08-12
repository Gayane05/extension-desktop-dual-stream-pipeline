// desktop/tests/test_ws_server.cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include "core/protocol.h"
#include "net/ws_server.h"

using namespace dsp;
using namespace std::chrono_literals;

namespace {
template <typename Pred>
bool waitFor(Pred p, std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return p();
}
}  // namespace

TEST(WsServer, HelloThenAudioReachesCallback) {
    ix::initNetSystem();
    std::atomic<int> frames{0};
    std::atomic<bool> gotHello{false};
    std::atomic<bool> gone{false};
    WsServer server(18765, {
        .onAudio = [&](AudioFrame&& f) { if (f.stream == StreamId::Mic) frames++; },
        .onHello = [&](const HelloInfo& h) { gotHello = h.sampleRate == 16000; },
        .onClientGone = [&] { gone = true; },
    });
    std::string err;
    ASSERT_TRUE(server.start(err)) << err;

    ix::WebSocket client;
    client.setUrl("ws://127.0.0.1:18765");
    std::atomic<bool> open{false};
    client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) open = true;
    });
    client.start();
    ASSERT_TRUE(waitFor([&] { return open.load(); }));

    client.sendText(R"({"type":"hello","version":1,"sampleRate":16000,"channels":1,"format":"s16le"})");
    std::vector<int16_t> pcm(1600, 0);
    auto frame = serializeBinaryFrame(StreamId::Mic, 123.0, pcm.data(), pcm.size());
    client.sendBinary(std::string(reinterpret_cast<char*>(frame.data()), frame.size()));

    EXPECT_TRUE(waitFor([&] { return gotHello.load() && frames.load() >= 1; }));

    client.sendText(R"({"type":"bye"})");
    EXPECT_TRUE(waitFor([&] { return gone.load(); }));
    client.stop();
    server.stop();
    ix::uninitNetSystem();
}

TEST(WsServer, AudioBeforeHelloIsDropped) {
    ix::initNetSystem();
    std::atomic<int> frames{0};
    WsServer server(18766, {
        .onAudio = [&](AudioFrame&&) { frames++; },
        .onHello = [](const HelloInfo&) {},
        .onClientGone = [] {},
    });
    std::string err;
    ASSERT_TRUE(server.start(err)) << err;

    ix::WebSocket client;
    client.setUrl("ws://127.0.0.1:18766");
    std::atomic<bool> open{false};
    client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) open = true;
    });
    client.start();
    ASSERT_TRUE(waitFor([&] { return open.load(); }));

    std::vector<int16_t> pcm(160, 0);
    auto frame = serializeBinaryFrame(StreamId::Mic, 1.0, pcm.data(), pcm.size());
    client.sendBinary(std::string(reinterpret_cast<char*>(frame.data()), frame.size()));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(frames.load(), 0);
    client.stop();
    server.stop();
    ix::uninitNetSystem();
}

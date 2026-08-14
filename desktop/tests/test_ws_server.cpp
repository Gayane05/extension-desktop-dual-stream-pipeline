// desktop/tests/test_ws_server.cpp
#include <gtest/gtest.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/protocol.h"
#include "net/ws_server.h"

using namespace dsp;
using namespace std::chrono_literals;

namespace {
template <typename Pred>
bool waitFor(Pred p, std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (p())
            return true;
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
    WsServer server(18765,
                    {
                        .onAudio =
                            [&](AudioFrame&& f) {
                                if (f.stream == StreamId::Mic)
                                    frames++;
                            },
                        .onHello = [&](const HelloInfo& h) { gotHello = h.sampleRate == 16000; },
                        .onClientGone = [&] { gone = true; },
                    });
    std::string err;
    ASSERT_TRUE(server.start(err)) << err;

    ix::WebSocket client;
    client.setUrl("ws://127.0.0.1:18765");
    std::atomic<bool> open{false};
    client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open)
            open = true;
    });
    client.start();
    ASSERT_TRUE(waitFor([&] { return open.load(); }));

    client.sendText(
        R"({"type":"hello","version":1,"sampleRate":16000,"channels":1,"format":"s16le"})");
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
        if (msg->type == ix::WebSocketMessageType::Open)
            open = true;
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

TEST(WsServer, BadHelloGetsErrorAndClose) {
    ix::initNetSystem();
    WsServer server(18767, {
                               .onAudio = [](AudioFrame&&) {},
                               .onHello = [](const HelloInfo&) {},
                               .onClientGone = [] {},
                           });
    std::string err;
    ASSERT_TRUE(server.start(err)) << err;

    ix::WebSocket client;
    client.setUrl("ws://127.0.0.1:18767");
    std::atomic<bool> open{false}, gotError{false}, closed{false};
    client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open)
            open = true;
        else if (msg->type == ix::WebSocketMessageType::Message && !msg->binary &&
                 msg->str.find("\"type\":\"error\"") != std::string::npos)
            gotError = true;
        else if (msg->type == ix::WebSocketMessageType::Close)
            closed = true;
    });
    client.disableAutomaticReconnection();  // so server-close doesn't trigger reconnect loops
    client.start();
    ASSERT_TRUE(waitFor([&] { return open.load(); }));

    client.sendText(
        R"({"type":"hello","version":1,"sampleRate":48000,"channels":1,"format":"s16le"})");  // wrong
                                                                                              // rate
    EXPECT_TRUE(waitFor([&] { return gotError.load() && closed.load(); }));
    client.stop();
    server.stop();
    ix::uninitNetSystem();
}

// The SPSC rings behind onAudio require a single producer per stream. A
// second client that sends hello while one is already active must be
// rejected (error + close) without disturbing the first client's stream.
TEST(WsServer, SecondClientHelloIsRejectedFirstKeepsStreaming) {
    ix::initNetSystem();
    std::atomic<int> framesA{0};
    std::atomic<int> helloCount{0};
    std::atomic<int> goneCount{0};
    WsServer server(18768, {
                               .onAudio =
                                   [&](AudioFrame&& f) {
                                       if (f.stream == StreamId::Mic)
                                           framesA++;
                                   },
                               .onHello = [&](const HelloInfo&) { helloCount++; },
                               .onClientGone = [&] { goneCount++; },
                           });
    std::string err;
    ASSERT_TRUE(server.start(err)) << err;

    ix::WebSocket a;
    a.setUrl("ws://127.0.0.1:18768");
    std::atomic<bool> aOpen{false};
    a.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open)
            aOpen = true;
    });
    a.start();
    ASSERT_TRUE(waitFor([&] { return aOpen.load(); }));
    a.sendText(R"({"type":"hello","version":1,"sampleRate":16000,"channels":1,"format":"s16le"})");
    EXPECT_TRUE(waitFor([&] { return helloCount.load() == 1; }));

    ix::WebSocket b;
    b.setUrl("ws://127.0.0.1:18768");
    std::atomic<bool> bOpen{false}, bGotError{false}, bClosed{false};
    b.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open)
            bOpen = true;
        else if (msg->type == ix::WebSocketMessageType::Message && !msg->binary &&
                 msg->str.find("\"type\":\"error\"") != std::string::npos)
            bGotError = true;
        else if (msg->type == ix::WebSocketMessageType::Close)
            bClosed = true;
    });
    b.disableAutomaticReconnection();
    b.start();
    ASSERT_TRUE(waitFor([&] { return bOpen.load(); }));
    b.sendText(R"({"type":"hello","version":1,"sampleRate":16000,"channels":1,"format":"s16le"})");
    EXPECT_TRUE(waitFor([&] { return bGotError.load() && bClosed.load(); }));
    b.stop();

    // B's rejection/close must not have been reported as A's onClientGone,
    // and must not have prevented a second hello from A (still the sole
    // active client, so helloCount should still read 1).
    EXPECT_EQ(helloCount.load(), 1);
    EXPECT_EQ(goneCount.load(), 0);

    // A keeps streaming after B was rejected.
    std::vector<int16_t> pcm(1600, 0);
    auto frame = serializeBinaryFrame(StreamId::Mic, 42.0, pcm.data(), pcm.size());
    a.sendBinary(std::string(reinterpret_cast<char*>(frame.data()), frame.size()));
    EXPECT_TRUE(waitFor([&] { return framesA.load() >= 1; }));

    a.sendText(R"({"type":"bye"})");
    EXPECT_TRUE(waitFor([&] { return goneCount.load() >= 1; }));
    a.stop();
    server.stop();
    ix::uninitNetSystem();
}

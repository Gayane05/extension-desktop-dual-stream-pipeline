// Streams two 16 kHz mono PCM16 WAVs over the extension's WS protocol.
// NOMINMAX must precede any header that drags in <windows.h> (ixwebsocket
// does, transitively): windows.h's min/max macros otherwise shadow
// std::min/std::max below and break compilation with cryptic C2589 errors.
#define NOMINMAX
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/protocol.h"
#include "core/wav.h"

using namespace dsp;
using namespace std::chrono;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: wav_client <mic.wav> <tab.wav> [port]\n");
        return 2;
    }
    std::string err;
    auto mic = readWavPcm16Mono(argv[1], err);
    if (!mic)
    {
        std::fprintf(stderr, "mic wav: %s\n", err.c_str());
        return 2;
    }
    else if (mic->sampleRate != kSampleRateHz)
    {
        std::fprintf(stderr, "mic wav: expected 16000 Hz, got %d\n", mic->sampleRate);
        return 2;
    }
    auto tab = readWavPcm16Mono(argv[2], err);
    if (!tab)
    {
        std::fprintf(stderr, "tab wav: %s\n", err.c_str());
        return 2;
    }
    else if (tab->sampleRate != kSampleRateHz)
    {
        std::fprintf(stderr, "tab wav: expected 16000 Hz, got %d\n", tab->sampleRate);
        return 2;
    }
    int port = argc > 3 ? std::atoi(argv[3]) : 8765;

    ix::initNetSystem();
    ix::WebSocket ws;
    ws.setUrl("ws://127.0.0.1:" + std::to_string(port));
    std::atomic<bool> open{false};
    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open)
        {
            open = true;
        }
        else if (msg->type == ix::WebSocketMessageType::Message && !msg->binary)
        {
            std::fprintf(stderr, "server: %s\n", msg->str.c_str());
        }
    });
    ws.start();
    for (int i = 0; i < 500 && !open; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (!open)
    {
        std::fprintf(stderr, "cannot connect to port %d\n", port);
        return 3;
    }

    ws.sendText(
        R"({"type":"hello","version":1,"sampleRate":16000,"channels":1,"format":"s16le","streams":["mic","tab"]})");

    constexpr size_t kChunkSamples = 1600;  // 100 ms.
    const size_t maxLen = std::max(mic->samples.size(), tab->samples.size());
    auto startTime = steady_clock::now();
    // Pace chunks against absolute deadlines (startTime + N*100ms) rather than
    // sleeping 100ms between iterations: a fixed per-iteration sleep
    // accumulates drift from the loop body's own execution time (and any
    // scheduler jitter), so after enough chunks the stream would run
    // measurably slower than real time. Anchoring every deadline to startTime
    // instead keeps long-run average pacing accurate, matching how the
    // extension feeds audio in real time.
    for (size_t off = 0; off < maxLen; off += kChunkSamples)
    {
        double tsMs = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        auto sendChunk = [&](StreamId id, const std::vector<int16_t>& pcm) {
            if (off >= pcm.size())
            {
                return;
            }
            size_t sampleCount = std::min(kChunkSamples, pcm.size() - off);
            auto frame = serializeBinaryFrame(id, tsMs, pcm.data() + off, sampleCount);
            ws.sendBinary(std::string(reinterpret_cast<char*>(frame.data()), frame.size()));
        };
        sendChunk(StreamId::Mic, mic->samples);
        sendChunk(StreamId::Tab, tab->samples);
        std::this_thread::sleep_until(startTime + milliseconds(100) * (off / kChunkSamples + 1));
    }
    // sherpa's endpoint rules (see sherpa_engine.cpp) finalize an utterance
    // only after observing a trailing silence gap, so without feeding a few
    // seconds of silence after the real audio ends, the last utterance in
    // the WAV would stay "pending" (interim) forever with no final emitted.
    // Trailing silence so endpointing fires finals.
    std::vector<int16_t> silence(kChunkSamples, 0);
    for (int i = 0; i < 30; ++i)
    {
        double tsMs = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        auto micFrame = serializeBinaryFrame(StreamId::Mic, tsMs, silence.data(), silence.size());
        auto tabFrame = serializeBinaryFrame(StreamId::Tab, tsMs, silence.data(), silence.size());
        ws.sendBinary(std::string(reinterpret_cast<char*>(micFrame.data()), micFrame.size()));
        ws.sendBinary(std::string(reinterpret_cast<char*>(tabFrame.data()), tabFrame.size()));
        std::this_thread::sleep_for(100ms);
    }
    ws.sendText(R"({"type":"bye"})");
    std::this_thread::sleep_for(200ms);
    ws.stop();
    ix::uninitNetSystem();
    std::fprintf(stderr, "done\n");
    return 0;
}

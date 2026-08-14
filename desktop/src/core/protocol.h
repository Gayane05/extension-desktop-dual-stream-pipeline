// desktop/src/core/protocol.h
//
// Wire format shared by the extension (JS) and the desktop app (C++): the
// binary AudioFrame layout and the small set of JSON control messages
// (hello/bye/status/error) exchanged over the WsServer connection. Consumed
// by net/ws_server.cpp (parses incoming frames/JSON) and app/pipeline.cpp
// (builds status JSON to broadcast). Keep this header's framing in sync with
// extension/offscreen.js's sendFrame()/connectWs() -- they encode/decode the
// same bytes independently, with no shared schema enforcement.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dsp {
enum class StreamId : uint8_t { Mic = 0, Tab = 1 };
inline const char* streamName(StreamId s)
{
    return s == StreamId::Mic ? "mic" : "tab";
}

struct AudioFrame {
    StreamId stream;
    double captureTsMs;
    std::vector<int16_t> samples;
};

// Binary frame layout (see protocol.cpp for parse/serialize): 1 byte stream
// tag (0=mic, 1=tab) + 8 bytes little-endian f64 capture timestamp (ms) +
// N*2 bytes of PCM16 mono samples at 16 kHz.
inline constexpr size_t kFrameHeaderSize = 9;

std::optional<AudioFrame> parseBinaryFrame(const uint8_t* data, size_t len);
std::vector<uint8_t> serializeBinaryFrame(StreamId s, double tsMs, const int16_t* samples,
                                          size_t n);

struct HelloInfo {
    int version = 0;
    int sampleRate = 0;
    int channels = 0;
    std::string format;
};
std::optional<HelloInfo> parseHello(const std::string& jsonText);
bool isBye(const std::string& jsonText);
std::string buildStatusJson(const std::string& engine, const std::string& provider,
                            const std::string& micState, const std::string& tabState);
std::string buildErrorJson(const std::string& message);
}  // namespace dsp

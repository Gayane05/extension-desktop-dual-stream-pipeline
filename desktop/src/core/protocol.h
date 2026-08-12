#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dsp {
enum class StreamId : uint8_t { Mic = 0, Tab = 1 };
inline const char* streamName(StreamId s) { return s == StreamId::Mic ? "mic" : "tab"; }

struct AudioFrame {
    StreamId stream;
    double captureTsMs;
    std::vector<int16_t> samples;
};

inline constexpr size_t kFrameHeaderSize = 9;

std::optional<AudioFrame> parseBinaryFrame(const uint8_t* data, size_t len);
std::vector<uint8_t> serializeBinaryFrame(StreamId s, double tsMs,
                                          const int16_t* samples, size_t n);

struct HelloInfo { int version = 0; int sampleRate = 0; int channels = 0; std::string format; };
std::optional<HelloInfo> parseHello(const std::string& jsonText);
bool isBye(const std::string& jsonText);
std::string buildStatusJson(const std::string& engine, const std::string& provider,
                            const std::string& micState, const std::string& tabState);
std::string buildErrorJson(const std::string& message);
}  // namespace dsp

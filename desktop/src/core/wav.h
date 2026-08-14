#pragma once
#include <optional>
#include <string>
#include <vector>

namespace dsp {
struct WavData {
    int sampleRate = 0;
    std::vector<int16_t> samples;
};
std::optional<WavData> readWavPcm16Mono(const std::string& path, std::string& error);
}  // namespace dsp

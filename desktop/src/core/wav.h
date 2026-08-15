// desktop/src/core/wav.h
//
// Minimal RIFF/WAVE reader for PCM16 mono files. Used only by
// desktop/tools/wav_client (offline replay of recorded audio through the
// same WS protocol the extension uses) -- not part of the live capture path.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace dsp
{
struct WavData
{
    int sampleRate = 0;
    std::vector<int16_t> samples;
};
std::optional<WavData> readWavPcm16Mono(const std::string& path, std::string& error);
}  // namespace dsp

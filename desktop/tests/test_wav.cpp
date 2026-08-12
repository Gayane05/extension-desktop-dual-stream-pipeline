#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include "core/wav.h"

using namespace dsp;

static std::string writeTinyWav(int sampleRate, const std::vector<int16_t>& pcm) {
    std::string path = std::string(::testing::TempDir()) + "tiny.wav";
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    uint32_t dataSize = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4); w32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(1); w32(sampleRate);
    w32(sampleRate * 2); w16(2); w16(16);
    f.write("data", 4); w32(dataSize);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataSize);
    return path;
}

TEST(Wav, ReadsPcm16Mono) {
    std::vector<int16_t> pcm{1, -1, 32767, -32768};
    auto path = writeTinyWav(16000, pcm);
    std::string err;
    auto wav = readWavPcm16Mono(path, err);
    ASSERT_TRUE(wav) << err;
    EXPECT_EQ(wav->sampleRate, 16000);
    EXPECT_EQ(wav->samples, pcm);
}

TEST(Wav, RejectsMissingFile) {
    std::string err;
    EXPECT_FALSE(readWavPcm16Mono("Z:/nope.wav", err));
}

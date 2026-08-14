#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include "core/wav.h"

using namespace dsp;

static std::string writeTinyWav(int sampleRate, const std::vector<int16_t>& pcm)
{
    std::string path = std::string(::testing::TempDir()) + "tiny.wav";
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    uint32_t dataSize = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4);
    w32(36 + dataSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w32(16);
    w16(1);
    w16(1);
    w32(sampleRate);
    w32(sampleRate * 2);
    w16(2);
    w16(16);
    f.write("data", 4);
    w32(dataSize);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataSize);
    return path;
}

TEST(Wav, ReadsPcm16Mono)
{
    std::vector<int16_t> pcm{1, -1, 32767, -32768};
    auto path = writeTinyWav(16000, pcm);
    std::string err;
    auto wav = readWavPcm16Mono(path, err);
    ASSERT_TRUE(wav) << err;
    EXPECT_EQ(wav->sampleRate, 16000);
    EXPECT_EQ(wav->samples, pcm);
}

TEST(Wav, RejectsMissingFile)
{
    std::string err;
    EXPECT_FALSE(readWavPcm16Mono("Z:/nope.wav", err));
}

// Regression test for an out-of-bounds read: a malformed "fmt " chunk that
// declares fewer than 16 bytes used to be read into a correctly-sized
// (undersized) buffer, then memcpy'd from fixed offsets up to +14 -- reading
// past the end of that buffer. The reader must reject it instead.
TEST(Wav, RejectsShortFmtChunk)
{
    std::string path = std::string(::testing::TempDir()) + "short_fmt.wav";
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    // fmt chunk of only 8 bytes: fmt(2) + channels(2) + rate(4), missing
    // byteRate/blockAlign/bitsPerSample -- short of the 16-byte minimum.
    f.write("RIFF", 4);
    w32(36);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w32(8);
    w16(1);
    w16(1);
    w32(16000);
    f.write("data", 4);
    w32(0);
    f.close();

    std::string err;
    auto wav = readWavPcm16Mono(path, err);
    EXPECT_FALSE(wav);
    EXPECT_NE(err.find("too small"), std::string::npos) << err;
}

// Regression test: a data chunk whose declared size exceeds the bytes
// actually present in the file used to be silently zero-padded by
// std::ifstream::read's short-read semantics; the reader must detect the
// truncation and fail instead of returning partially-garbage audio.
TEST(Wav, RejectsTruncatedDataChunk)
{
    std::vector<int16_t> pcm{1, -1, 32767, -32768};
    auto path = writeTinyWav(16000, pcm);
    // Rewrite the data-chunk size field (RIFF header at offset 4, "fmt "
    // chunk header+body 8+16=24, "data" id+size at offset 12+24=36) to claim
    // twice as many bytes as were actually written, without adding them.
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f);
    uint32_t claimedSize = static_cast<uint32_t>(pcm.size() * 2 * 2);
    f.seekp(4 + 4 + 4 + 4 + 4 + 2 + 2 + 4 + 4 + 2 + 2 + 4);  // start of data-size field
    f.write(reinterpret_cast<char*>(&claimedSize), 4);
    f.close();

    std::string err;
    auto wav = readWavPcm16Mono(path, err);
    EXPECT_FALSE(wav);
    EXPECT_NE(err.find("truncated"), std::string::npos) << err;
}

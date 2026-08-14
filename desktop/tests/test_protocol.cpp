// desktop/tests/test_protocol.cpp
//
// Unit tests for core/protocol.h's wire codec: binary frame round-trip and
// malformed-input rejection, plus the hello/bye/status/error JSON helpers.
#include <gtest/gtest.h>

#include <cstring>
#include <limits>

#include "core/protocol.h"

using namespace dsp;

TEST(Protocol, RoundTripBinaryFrame)
{
    std::vector<int16_t> pcm{100, -200, 32767, -32768};
    auto bytes = serializeBinaryFrame(StreamId::Tab, 1723456789123.5, pcm.data(), pcm.size());
    ASSERT_EQ(bytes.size(), 9 + pcm.size() * 2);
    auto frame = parseBinaryFrame(bytes.data(), bytes.size());
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->stream, StreamId::Tab);
    EXPECT_DOUBLE_EQ(frame->captureTsMs, 1723456789123.5);
    EXPECT_EQ(frame->samples, pcm);
}

TEST(Protocol, RejectsTruncatedAndBadTag)
{
    std::vector<int16_t> pcm{1};
    auto ok = serializeBinaryFrame(StreamId::Mic, 1.0, pcm.data(), pcm.size());
    EXPECT_FALSE(parseBinaryFrame(ok.data(), 8).has_value());   // shorter than header
    EXPECT_FALSE(parseBinaryFrame(ok.data(), 10).has_value());  // odd payload length
    ok[0] = 7;                                                  // unknown tag
    EXPECT_FALSE(parseBinaryFrame(ok.data(), ok.size()).has_value());
}

TEST(Protocol, RejectsNonFiniteCaptureTs)
{
    std::vector<int16_t> pcm{1, 2, 3};
    auto bytes = serializeBinaryFrame(StreamId::Mic, 1.0, pcm.data(), pcm.size());
    ASSERT_TRUE(parseBinaryFrame(bytes.data(), bytes.size()).has_value());  // sanity: valid as-is
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::memcpy(bytes.data() + 1, &nan, sizeof(double));  // overwrite captureTsMs bytes [1,9)
    EXPECT_FALSE(parseBinaryFrame(bytes.data(), bytes.size()).has_value());
}

TEST(Protocol, EmptyPayloadIsValid)
{
    auto bytes = serializeBinaryFrame(StreamId::Mic, 2.0, nullptr, 0);
    auto frame = parseBinaryFrame(bytes.data(), bytes.size());
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->samples.empty());
}

TEST(Protocol, ParsesHello)
{
    auto hello = parseHello(
        R"({"type":"hello","version":1,"sampleRate":16000,"channels":1,"format":"s16le","streams":["mic","tab"]})");
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->version, 1);
    EXPECT_EQ(hello->sampleRate, 16000);
    EXPECT_EQ(hello->channels, 1);
    EXPECT_EQ(hello->format, "s16le");
}

TEST(Protocol, RejectsNonHelloAndGarbage)
{
    EXPECT_FALSE(parseHello(R"({"type":"bye"})").has_value());
    EXPECT_FALSE(parseHello("not json at all").has_value());
    EXPECT_TRUE(isBye(R"({"type":"bye"})"));
    EXPECT_FALSE(isBye(R"({"type":"hello"})"));
}

TEST(Protocol, BuildsStatusJson)
{
    auto statusJson = buildStatusJson("sherpa", "cpu", "streaming", "idle");
    EXPECT_NE(statusJson.find("\"type\":\"status\""), std::string::npos);
    EXPECT_NE(statusJson.find("\"engine\":\"sherpa\""), std::string::npos);
    EXPECT_NE(statusJson.find("\"mic\":\"streaming\""), std::string::npos);
    EXPECT_NE(statusJson.find("\"tab\":\"idle\""), std::string::npos);
}

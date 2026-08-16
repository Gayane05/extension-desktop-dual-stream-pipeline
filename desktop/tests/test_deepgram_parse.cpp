// desktop/tests/test_deepgram_parse.cpp
//
// Unit tests for parseDeepgramMessage (stt/deepgram_engine.h): interim vs
// final "Results" JSON, utterance-start timestamping (connection epoch plus
// the result's "start" offset), and rejection of unrelated/malformed
// messages.
#include <gtest/gtest.h>

#include "stt/deepgram_engine.h"

using namespace dsp;

static const char* kInterim = R"({
  "type":"Results","channel_index":[0,1],"duration":1.0,"start":0.0,"is_final":false,
  "channel":{"alternatives":[{"transcript":"hello wor","confidence":0.9,"words":[]}]}
})";
static const char* kFinal = R"({
  "type":"Results","channel_index":[0,1],"duration":1.2,"start":0.0,"is_final":true,
  "channel":{"alternatives":[{"transcript":"hello world","confidence":0.95,"words":[]}]}
})";
static const char* kMeta = R"({"type":"Metadata","request_id":"x"})";

TEST(DeepgramParse, Interim)
{
    // "start" is 0.0 here, so the event timestamp equals the epoch.
    auto ev = parseDeepgramMessage(StreamId::Tab, kInterim, 5000.0);
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->stream, StreamId::Tab);
    EXPECT_EQ(ev->text, "hello wor");
    EXPECT_FALSE(ev->isFinal);
    EXPECT_DOUBLE_EQ(ev->tsMs, 5000.0);
}

TEST(DeepgramParse, TimestampIsEpochPlusUtteranceStart)
{
    static const char* kLaterUtterance = R"({
      "type":"Results","channel_index":[0,1],"duration":1.0,"start":2.5,"is_final":true,
      "channel":{"alternatives":[{"transcript":"later words","confidence":0.9,"words":[]}]}
    })";
    auto ev = parseDeepgramMessage(StreamId::Mic, kLaterUtterance, 1000000.0);
    ASSERT_TRUE(ev);
    // 2.5 s into the stream on top of the connection epoch.
    EXPECT_DOUBLE_EQ(ev->tsMs, 1002500.0);
}

TEST(DeepgramParse, Final)
{
    auto ev = parseDeepgramMessage(StreamId::Mic, kFinal, 6000.0);
    ASSERT_TRUE(ev);
    EXPECT_TRUE(ev->isFinal);
    EXPECT_EQ(ev->text, "hello world");
}

TEST(DeepgramParse, IgnoresMetadataEmptyAndGarbage)
{
    EXPECT_FALSE(parseDeepgramMessage(StreamId::Mic, kMeta, 0.0));
    EXPECT_FALSE(parseDeepgramMessage(StreamId::Mic, "{}", 0.0));
    EXPECT_FALSE(parseDeepgramMessage(StreamId::Mic, "garbage", 0.0));
    // Empty transcript (silence) produces no event.
    EXPECT_FALSE(parseDeepgramMessage(
        StreamId::Mic,
        R"({"type":"Results","is_final":false,"channel":{"alternatives":[{"transcript":""}]}})",
        0.0));
}

TEST(DeepgramUrl, MultiAddsEndpointingAndSpecificLanguageDoesNot)
{
    const std::string multiUrl = buildDeepgramListenUrl("multi");
    EXPECT_NE(multiUrl.find("model=nova-3"), std::string::npos);
    EXPECT_NE(multiUrl.find("language=multi"), std::string::npos);
    // Deepgram's recommended endpointing for code-switching applies to
    // multi only.
    EXPECT_NE(multiUrl.find("endpointing=100"), std::string::npos);
    const std::string englishUrl = buildDeepgramListenUrl("en");
    EXPECT_NE(englishUrl.find("language=en"), std::string::npos);
    EXPECT_EQ(englishUrl.find("endpointing"), std::string::npos);
    EXPECT_NE(englishUrl.find("sample_rate=16000"), std::string::npos);
}

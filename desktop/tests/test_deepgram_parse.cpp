// desktop/tests/test_deepgram_parse.cpp
//
// Unit tests for parseDeepgramMessage (stt/deepgram_engine.h): interim vs
// final "Results" JSON, and rejection of unrelated/malformed messages.
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
    auto ev = parseDeepgramMessage(StreamId::Tab, kInterim, 5000.0);
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->stream, StreamId::Tab);
    EXPECT_EQ(ev->text, "hello wor");
    EXPECT_FALSE(ev->isFinal);
    EXPECT_DOUBLE_EQ(ev->tsMs, 5000.0);
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
    // empty transcript (silence) produces no event
    EXPECT_FALSE(parseDeepgramMessage(
        StreamId::Mic,
        R"({"type":"Results","is_final":false,"channel":{"alternatives":[{"transcript":""}]}})",
        0.0));
}

#include <gtest/gtest.h>
#include "app/transcript_model.h"

using namespace dsp;

TEST(TranscriptModel, InterimReplacedInPlace) {
    TranscriptModel m;
    m.apply({StreamId::Mic, "hel", false, 100.0});
    m.apply({StreamId::Mic, "hello wor", false, 100.0});
    auto s = m.snapshot();
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].text, "hello wor");
    EXPECT_FALSE(s[0].isFinal);
}

TEST(TranscriptModel, FinalCommitsAndClearsPending) {
    TranscriptModel m;
    m.apply({StreamId::Mic, "hello", false, 100.0});
    m.apply({StreamId::Mic, "hello world", true, 100.0});
    auto s = m.snapshot();
    ASSERT_EQ(s.size(), 1u);
    EXPECT_TRUE(s[0].isFinal);
    EXPECT_EQ(s[0].text, "hello world");
}

TEST(TranscriptModel, LanesInterleaveChronologically) {
    TranscriptModel m;
    m.apply({StreamId::Tab, "how are you", true, 200.0});
    m.apply({StreamId::Mic, "fine thanks", true, 300.0});
    m.apply({StreamId::Tab, "good", true, 400.0});
    m.apply({StreamId::Mic, "typing something", false, 500.0});  // pending shown last
    auto s = m.snapshot();
    ASSERT_EQ(s.size(), 4u);
    EXPECT_EQ(s[0].stream, StreamId::Tab);
    EXPECT_EQ(s[1].stream, StreamId::Mic);
    EXPECT_EQ(s[2].text, "good");
    EXPECT_FALSE(s[3].isFinal);
}

TEST(TranscriptModel, OutOfOrderFinalInsertsSorted) {
    TranscriptModel m;
    m.apply({StreamId::Mic, "later", true, 900.0});
    m.apply({StreamId::Tab, "earlier", true, 100.0});
    auto s = m.snapshot();
    EXPECT_EQ(s[0].text, "earlier");
    EXPECT_EQ(s[1].text, "later");
}

TEST(TranscriptModel, EmptyFinalIgnored) {
    TranscriptModel m;
    m.apply({StreamId::Mic, "", true, 100.0});
    EXPECT_TRUE(m.snapshot().empty());
}

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "app/transcript_model.h"

using namespace dsp;

TEST(TranscriptModel, InterimReplacedInPlace)
{
    TranscriptModel m;
    m.apply({StreamId::Mic, "hel", false, 100.0});
    m.apply({StreamId::Mic, "hello wor", false, 100.0});
    auto s = m.snapshot();
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].text, "hello wor");
    EXPECT_FALSE(s[0].isFinal);
}

TEST(TranscriptModel, FinalCommitsAndClearsPending)
{
    TranscriptModel m;
    m.apply({StreamId::Mic, "hello", false, 100.0});
    m.apply({StreamId::Mic, "hello world", true, 100.0});
    auto s = m.snapshot();
    ASSERT_EQ(s.size(), 1u);
    EXPECT_TRUE(s[0].isFinal);
    EXPECT_EQ(s[0].text, "hello world");
}

TEST(TranscriptModel, LanesInterleaveChronologically)
{
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

TEST(TranscriptModel, OutOfOrderFinalInsertsSorted)
{
    TranscriptModel m;
    m.apply({StreamId::Mic, "later", true, 900.0});
    m.apply({StreamId::Tab, "earlier", true, 100.0});
    auto s = m.snapshot();
    EXPECT_EQ(s[0].text, "earlier");
    EXPECT_EQ(s[1].text, "later");
}

TEST(TranscriptModel, EmptyFinalIgnored)
{
    TranscriptModel m;
    m.apply({StreamId::Mic, "", true, 100.0});
    EXPECT_TRUE(m.snapshot().empty());
}

TEST(TranscriptModel, ToTextFormatsFinalsOnly)
{
    TranscriptModel m;
    // 1h 2m 3s = 3723000 ms; 1h 2m 4s = 3724000 ms
    m.apply({StreamId::Mic, "hello there", true, 3723000.0});
    m.apply({StreamId::Tab, "hi yourself", true, 3724000.0});
    m.apply({StreamId::Mic, "pending interim", false, 3725000.0});  // must NOT appear
    EXPECT_EQ(m.toText(),
              "[01:02:03] You: hello there\n"
              "[01:02:04] Others: hi yourself\n");
}

TEST(TranscriptModel, ConcurrentApplyAndSnapshotSmoke)
{
    TranscriptModel m;
    constexpr int kN = 5000;
    std::thread micWriter([&] {
        for (int i = 0; i < kN; ++i)
            m.apply({StreamId::Mic, "mic " + std::to_string(i), i % 10 == 9, i * 10.0});
    });
    std::thread tabWriter([&] {
        for (int i = 0; i < kN; ++i)
            m.apply({StreamId::Tab, "tab " + std::to_string(i), i % 10 == 9, i * 10.0 + 5.0});
    });
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done)
        {
            auto s = m.snapshot();
            (void)s;
        }
    });
    micWriter.join();
    tabWriter.join();
    done = true;
    reader.join();
    auto s = m.snapshot();
    EXPECT_EQ(s.size(), kN / 10 * 2u);  // 500 finals per lane
    for (size_t i = 1; i < s.size(); ++i)
        EXPECT_LE(s[i - 1].tsMs, s[i].tsMs);  // sorted invariant held
}

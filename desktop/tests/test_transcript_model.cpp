// desktop/tests/test_transcript_model.cpp
//
// Unit tests for app/transcript_model.h: interim-replacement semantics,
// sorted-final insertion by timestamp, and snapshot/clear/toText behavior.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "app/transcript_model.h"

using namespace dsp;

TEST(TranscriptModel, InterimReplacedInPlace)
{
    TranscriptModel model;
    model.apply({StreamId::Mic, "hel", false, 100.0});
    model.apply({StreamId::Mic, "hello wor", false, 100.0});
    auto entries = model.snapshot();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].text, "hello wor");
    EXPECT_FALSE(entries[0].isFinal);
}

TEST(TranscriptModel, FinalCommitsAndClearsPending)
{
    TranscriptModel model;
    model.apply({StreamId::Mic, "hello", false, 100.0});
    model.apply({StreamId::Mic, "hello world", true, 100.0});
    auto entries = model.snapshot();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].isFinal);
    EXPECT_EQ(entries[0].text, "hello world");
}

TEST(TranscriptModel, LanesInterleaveChronologically)
{
    TranscriptModel model;
    model.apply({StreamId::Tab, "how are you", true, 200.0});
    model.apply({StreamId::Mic, "fine thanks", true, 300.0});
    model.apply({StreamId::Tab, "good", true, 400.0});
    model.apply({StreamId::Mic, "typing something", false, 500.0});  // pending shown last
    auto entries = model.snapshot();
    ASSERT_EQ(entries.size(), 4u);
    EXPECT_EQ(entries[0].stream, StreamId::Tab);
    EXPECT_EQ(entries[1].stream, StreamId::Mic);
    EXPECT_EQ(entries[2].text, "good");
    EXPECT_FALSE(entries[3].isFinal);
}

TEST(TranscriptModel, OutOfOrderFinalInsertsSorted)
{
    TranscriptModel model;
    model.apply({StreamId::Mic, "later", true, 900.0});
    model.apply({StreamId::Tab, "earlier", true, 100.0});
    auto entries = model.snapshot();
    EXPECT_EQ(entries[0].text, "earlier");
    EXPECT_EQ(entries[1].text, "later");
}

TEST(TranscriptModel, EmptyFinalIgnored)
{
    TranscriptModel model;
    model.apply({StreamId::Mic, "", true, 100.0});
    EXPECT_TRUE(model.snapshot().empty());
}

TEST(TranscriptModel, ToTextFormatsFinalsOnly)
{
    TranscriptModel model;
    // 1h 2m 3s = 3723000 ms; 1h 2m 4s = 3724000 ms
    model.apply({StreamId::Mic, "hello there", true, 3723000.0});
    model.apply({StreamId::Tab, "hi yourself", true, 3724000.0});
    model.apply({StreamId::Mic, "pending interim", false, 3725000.0});  // must NOT appear
    EXPECT_EQ(model.toText(),
              "[01:02:03] You: hello there\n"
              "[01:02:04] Others: hi yourself\n");
}

TEST(TranscriptModel, ConcurrentApplyAndSnapshotSmoke)
{
    TranscriptModel model;
    constexpr int kN = 5000;
    std::thread micWriter([&] {
        for (int i = 0; i < kN; ++i)
        {
            model.apply({StreamId::Mic, "mic " + std::to_string(i), i % 10 == 9, i * 10.0});
        }
    });
    std::thread tabWriter([&] {
        for (int i = 0; i < kN; ++i)
        {
            model.apply({StreamId::Tab, "tab " + std::to_string(i), i % 10 == 9, i * 10.0 + 5.0});
        }
    });
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done)
        {
            auto entries = model.snapshot();
            (void)entries;
        }
    });
    micWriter.join();
    tabWriter.join();
    done = true;
    reader.join();
    auto entries = model.snapshot();
    EXPECT_EQ(entries.size(), kN / 10 * 2u);  // 500 finals per lane
    for (size_t i = 1; i < entries.size(); ++i)
    {
        EXPECT_LE(entries[i - 1].tsMs, entries[i].tsMs);  // sorted invariant held
    }
}

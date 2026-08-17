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
    model.apply({StreamId::Mic, "typing something", false, 500.0});  // Pending shown last.
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

TEST(TranscriptModel, ToTextFormatsFinalsOnlyRelativeToSessionStart)
{
    TranscriptModel model;
    // The absolute capture clock (here starting at 5000000 ms) must not leak
    // into the output: timestamps read relative to the session's first event.
    model.apply({StreamId::Mic, "hello there", true, 5000000.0});
    model.apply({StreamId::Tab, "hi yourself", true, 5004000.0});
    model.apply({StreamId::Mic, "pending interim", false, 5005000.0});  // Must NOT appear.
    EXPECT_EQ(model.toText(),
              "[00:00.000] You: hello there\n"
              "[00:04.000] Others: hi yourself\n");
}

TEST(TranscriptModel, FormatRelativeTimestampRollsToHours)
{
    EXPECT_EQ(formatRelativeTimestamp(1000.0, 1000.0), "00:00.000");
    EXPECT_EQ(formatRelativeTimestamp(193480.0, 1000.0), "03:12.480");
    // 1h 2m 3s past the baseline switches to the h:mm:ss form.
    EXPECT_EQ(formatRelativeTimestamp(3723450.0, 0.0), "1:02:03.450");
    // A timestamp before the baseline clamps to zero instead of underflowing.
    EXPECT_EQ(formatRelativeTimestamp(500.0, 1000.0), "00:00.000");
}

TEST(TranscriptModel, BaselineComesFromEarliestEventIncludingInterims)
{
    TranscriptModel model;
    EXPECT_DOUBLE_EQ(model.baseTsMs(), 0.0);                       // No events yet.
    model.apply({StreamId::Mic, "still talking", false, 7000.0});  // Interim first.
    model.apply({StreamId::Mic, "still talking now", true, 7000.0});
    model.apply({StreamId::Tab, "reply", true, 9000.0});
    EXPECT_DOUBLE_EQ(model.baseTsMs(), 7000.0);
    model.clear();
    EXPECT_DOUBLE_EQ(model.baseTsMs(), 0.0);  // Clear starts a fresh session.
}

TEST(TranscriptModel, ToSrtEmitsNumberedCuesEndingAtTheNextCue)
{
    TranscriptModel model;
    model.apply({StreamId::Mic, "hello everyone", true, 10000.0});
    model.apply({StreamId::Tab, "yes we hear you", true, 14300.0});
    EXPECT_EQ(model.toSrt(),
              "1\n"
              "00:00:00,000 --> 00:00:04,300\n"
              "[You] hello everyone\n"
              "\n"
              "2\n"
              "00:00:04,300 --> 00:00:07,300\n"  // Last cue: fixed 3 s duration.
              "[Others] yes we hear you\n"
              "\n");
}

TEST(TranscriptModel, ToVttUsesWebVttHeaderAndDotMilliseconds)
{
    TranscriptModel model;
    model.apply({StreamId::Tab, "welcome", true, 2000.0});
    EXPECT_EQ(model.toVtt(),
              "WEBVTT\n"
              "\n"
              "00:00:00.000 --> 00:00:03.000\n"
              "[Others] welcome\n"
              "\n");
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
    EXPECT_EQ(entries.size(), kN / 10 * 2u);  // 500 finals per lane.
    for (size_t i = 1; i < entries.size(); ++i)
    {
        EXPECT_LE(entries[i - 1].tsMs, entries[i].tsMs);  // Sorted invariant held.
    }
}

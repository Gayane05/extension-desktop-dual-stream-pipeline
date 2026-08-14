#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "ui/save_transcript.h"

using namespace dsp;

TEST(SaveTranscript, WritesFullContentOnSuccess) {
    const std::string path = std::string(::testing::TempDir()) + "saved_transcript.txt";
    const std::string text = "You: hello there\nOthers: hi back\n";

    std::string err;
    ASSERT_TRUE(saveTranscriptFile(path, text, err)) << err;
    EXPECT_TRUE(err.empty());

    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f);
    std::ostringstream ss;
    ss << f.rdbuf();
    EXPECT_EQ(ss.str(), text);
}

// Regression test for the review finding: fopen_s failure must be surfaced
// to the caller (not silently skipped), with a non-empty, human-readable
// reason -- a nonexistent drive is a reliable, environment-independent way
// to force an open failure (see Wav.RejectsMissingFile for the same trick).
TEST(SaveTranscript, ReportsErrorWhenOpenFails) {
    std::string err;
    EXPECT_FALSE(saveTranscriptFile("Z:/nope/transcript.txt", "some text", err));
    EXPECT_FALSE(err.empty());
}

TEST(SaveTranscript, HandlesEmptyText) {
    const std::string path = std::string(::testing::TempDir()) + "saved_empty_transcript.txt";
    std::string err;
    ASSERT_TRUE(saveTranscriptFile(path, "", err)) << err;

    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f);
    std::ostringstream ss;
    ss << f.rdbuf();
    EXPECT_EQ(ss.str(), "");
}

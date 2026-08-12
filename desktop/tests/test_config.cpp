#include <gtest/gtest.h>
#include "app/config.h"

using namespace dsp;

static std::optional<Config> parse(std::vector<const char*> a, std::string& err) {
    a.insert(a.begin(), "transcriber.exe");
    return parseArgs(static_cast<int>(a.size()), a.data(), err);
}

TEST(Config, Defaults) {
    std::string err;
    auto c = parse({}, err);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->engine, "sherpa");
    EXPECT_EQ(c->provider, "cpu");
    EXPECT_EQ(c->port, 8765);
    EXPECT_FALSE(c->headless);
}

TEST(Config, ParsesAllFlags) {
    std::string err;
    auto c = parse({"--engine", "deepgram", "--provider", "cuda", "--port", "9000",
                    "--model-dir", "D:/models", "--headless", "--duration", "12.5"}, err);
    ASSERT_TRUE(c) << err;
    EXPECT_EQ(c->engine, "deepgram");
    EXPECT_EQ(c->provider, "cuda");
    EXPECT_EQ(c->port, 9000);
    EXPECT_EQ(c->modelDir, "D:/models");
    EXPECT_TRUE(c->headless);
    EXPECT_DOUBLE_EQ(c->durationSec, 12.5);
}

TEST(Config, RejectsBadValues) {
    std::string err;
    EXPECT_FALSE(parse({"--engine", "whisper"}, err));
    EXPECT_FALSE(parse({"--provider", "opencl"}, err));
    EXPECT_FALSE(parse({"--port", "notanumber"}, err));
    EXPECT_FALSE(parse({"--port"}, err));  // missing value
    EXPECT_FALSE(parse({"--unknown-flag"}, err));
}

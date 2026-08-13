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
    EXPECT_EQ(c->decoding, "beam");
    EXPECT_FALSE(c->headless);
}

TEST(Config, ParsesAllFlags) {
    std::string err;
    auto c = parse({"--engine", "deepgram", "--provider", "cuda", "--port", "9000",
                    "--model-dir", "D:/models", "--decoding", "greedy",
                    "--headless", "--duration", "12.5"}, err);
    ASSERT_TRUE(c) << err;
    EXPECT_EQ(c->engine, "deepgram");
    EXPECT_EQ(c->provider, "cuda");
    EXPECT_EQ(c->port, 9000);
    EXPECT_EQ(c->modelDir, "D:/models");
    EXPECT_EQ(c->decoding, "greedy");
    EXPECT_TRUE(c->headless);
    EXPECT_DOUBLE_EQ(c->durationSec, 12.5);
}

TEST(Config, RejectsBadValues) {
    std::string err;
    EXPECT_FALSE(parse({"--engine", "whisper"}, err));
    EXPECT_FALSE(parse({"--provider", "opencl"}, err));
    EXPECT_FALSE(parse({"--decoding", "fast"}, err));
    EXPECT_FALSE(parse({"--port", "notanumber"}, err));
    EXPECT_FALSE(parse({"--port"}, err));  // missing value
    EXPECT_FALSE(parse({"--unknown-flag"}, err));
}

TEST(Config, RejectsOutOfRangePorts) {
    std::string err;
    EXPECT_FALSE(parse({"--port", "0"}, err));
    EXPECT_FALSE(parse({"--port", "-1"}, err));
    EXPECT_FALSE(parse({"--port", "65536"}, err));
    EXPECT_TRUE(parse({"--port", "65535"}, err));   // boundary: valid
    EXPECT_TRUE(parse({"--port", "1"}, err));       // boundary: valid
}

TEST(Config, RejectsPartialNumericPort) {
    std::string err;
    EXPECT_FALSE(parse({"--port", "8080x"}, err));  // trailing garbage: full-string parse branch
}

TEST(Config, RejectsMissingValuesForAllValuedFlags) {
    std::string err;
    EXPECT_FALSE(parse({"--engine"}, err));
    EXPECT_FALSE(parse({"--provider"}, err));
    EXPECT_FALSE(parse({"--model-dir"}, err));
    EXPECT_FALSE(parse({"--duration"}, err));
}

TEST(Config, RejectsNonNumericDuration) {
    std::string err;
    EXPECT_FALSE(parse({"--duration", "abc"}, err));
}

TEST(Config, LenientDurationParsing) {
    std::string err;
    // std::stod parses leading numeric portion and does NOT throw
    // for "12abc", so it silently accepts as 12.0. This is documented
    // behavior difference from port flag's strict full-string parse.
    auto c = parse({"--duration", "12abc"}, err);
    EXPECT_TRUE(c);
    EXPECT_DOUBLE_EQ(c->durationSec, 12.0);
}

// desktop/tests/test_config.cpp
//
// Unit tests for app/config.h's CLI argv parser (parseArgs): defaults, valid
// flag values, rejection of invalid/missing ones, and the settings.json
// persistence used by the GUI's first-run chooser (load/save round trip,
// defaults-file-CLI precedence).
#include <gtest/gtest.h>

#include <fstream>

#include "app/config.h"

using namespace dsp;

static std::optional<Config> parse(std::vector<const char*> a, std::string& err)
{
    a.insert(a.begin(), "transcriber.exe");
    return parseArgs(static_cast<int>(a.size()), a.data(), err);
}

TEST(Config, Defaults)
{
    std::string err;
    auto c = parse({}, err);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->engine, "sherpa");
    EXPECT_EQ(c->provider, "cpu");
    EXPECT_EQ(c->port, 8765);
    EXPECT_EQ(c->decoding, "beam");
    EXPECT_DOUBLE_EQ(c->endpointSilenceSec, 0.8);
    EXPECT_FALSE(c->headless);
    EXPECT_FALSE(c->engineOrProviderExplicit);
}

TEST(Config, SettingsFileRoundTrip)
{
    Config out;
    out.engine = "deepgram";
    out.provider = "cuda";
    const std::string path = std::string(::testing::TempDir()) + "settings-roundtrip.json";
    ASSERT_TRUE(saveSettingsFile(path, out));
    Config in;
    ASSERT_TRUE(loadSettingsFile(path, in));
    EXPECT_EQ(in.engine, "deepgram");
    EXPECT_EQ(in.provider, "cuda");
}

TEST(Config, SettingsFileMissingLeavesDefaults)
{
    Config in;
    EXPECT_FALSE(loadSettingsFile("Z:/definitely/missing/settings.json", in));
    EXPECT_EQ(in.engine, "sherpa");
    EXPECT_EQ(in.provider, "cpu");
}

TEST(Config, SettingsFileInvalidValuesIgnored)
{
    const std::string path = std::string(::testing::TempDir()) + "settings-bad.json";
    std::ofstream(path) << R"({"engine":"whisper","provider":"cuda"})";
    Config in;
    EXPECT_TRUE(loadSettingsFile(path, in));
    EXPECT_EQ(in.engine, "sherpa");  // invalid engine value fell back to default
    EXPECT_EQ(in.provider, "cuda");  // valid provider still applied
}

TEST(Config, CliOverridesSettingsFileBase)
{
    Config base;
    base.engine = "deepgram";
    base.provider = "cuda";
    std::vector<const char*> a{"transcriber.exe", "--engine", "sherpa"};
    std::string err;
    auto c = parseArgs(static_cast<int>(a.size()), a.data(), err, base);
    ASSERT_TRUE(c) << err;
    EXPECT_EQ(c->engine, "sherpa");  // explicit CLI flag wins over the file
    EXPECT_EQ(c->provider, "cuda");  // untouched file value survives
    EXPECT_TRUE(c->engineOrProviderExplicit);
}

TEST(Config, ParsesAllFlags)
{
    std::string err;
    auto c = parse(
        {"--engine", "deepgram", "--provider", "cuda", "--port", "9000", "--model-dir", "D:/models",
         "--decoding", "greedy", "--endpoint-silence", "0.5", "--headless", "--duration", "12.5"},
        err);
    ASSERT_TRUE(c) << err;
    EXPECT_EQ(c->engine, "deepgram");
    EXPECT_EQ(c->provider, "cuda");
    EXPECT_EQ(c->port, 9000);
    EXPECT_EQ(c->modelDir, "D:/models");
    EXPECT_EQ(c->decoding, "greedy");
    EXPECT_DOUBLE_EQ(c->endpointSilenceSec, 0.5);
    EXPECT_TRUE(c->headless);
    EXPECT_DOUBLE_EQ(c->durationSec, 12.5);
}

TEST(Config, RejectsBadValues)
{
    std::string err;
    EXPECT_FALSE(parse({"--engine", "whisper"}, err));
    EXPECT_FALSE(parse({"--provider", "opencl"}, err));
    EXPECT_FALSE(parse({"--decoding", "fast"}, err));
    EXPECT_FALSE(parse({"--endpoint-silence", "abc"}, err));
    EXPECT_FALSE(parse({"--endpoint-silence", "0.1"}, err));  // below range
    EXPECT_FALSE(parse({"--endpoint-silence", "6"}, err));    // above range
    EXPECT_FALSE(parse({"--port", "notanumber"}, err));
    EXPECT_FALSE(parse({"--port"}, err));  // missing value
    EXPECT_FALSE(parse({"--unknown-flag"}, err));
}

TEST(Config, RejectsOutOfRangePorts)
{
    std::string err;
    EXPECT_FALSE(parse({"--port", "0"}, err));
    EXPECT_FALSE(parse({"--port", "-1"}, err));
    EXPECT_FALSE(parse({"--port", "65536"}, err));
    EXPECT_TRUE(parse({"--port", "65535"}, err));  // boundary: valid
    EXPECT_TRUE(parse({"--port", "1"}, err));      // boundary: valid
}

TEST(Config, RejectsPartialNumericPort)
{
    std::string err;
    EXPECT_FALSE(parse({"--port", "8080x"}, err));  // trailing garbage: full-string parse branch
}

TEST(Config, RejectsMissingValuesForAllValuedFlags)
{
    std::string err;
    EXPECT_FALSE(parse({"--engine"}, err));
    EXPECT_FALSE(parse({"--provider"}, err));
    EXPECT_FALSE(parse({"--model-dir"}, err));
    EXPECT_FALSE(parse({"--duration"}, err));
}

TEST(Config, RejectsNonNumericDuration)
{
    std::string err;
    EXPECT_FALSE(parse({"--duration", "abc"}, err));
}

TEST(Config, LenientDurationParsing)
{
    std::string err;
    // std::stod parses leading numeric portion and does NOT throw
    // for "12abc", so it silently accepts as 12.0. This is documented
    // behavior difference from port flag's strict full-string parse.
    auto c = parse({"--duration", "12abc"}, err);
    EXPECT_TRUE(c);
    EXPECT_DOUBLE_EQ(c->durationSec, 12.0);
}

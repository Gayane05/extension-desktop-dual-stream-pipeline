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

static std::optional<Config> parse(std::vector<const char*> args, std::string& err)
{
    args.insert(args.begin(), "transcriber.exe");
    return parseArgs(static_cast<int>(args.size()), args.data(), err);
}

TEST(Config, Defaults)
{
    std::string err;
    auto config = parse({}, err);
    ASSERT_TRUE(config);
    EXPECT_EQ(config->engine, "deepgram");
    EXPECT_EQ(config->provider, "cpu");
    EXPECT_EQ(config->port, 8765);
    EXPECT_EQ(config->decoding, "beam");
    EXPECT_DOUBLE_EQ(config->endpointSilenceSec, 0.8);
    EXPECT_FALSE(config->headless);
    EXPECT_FALSE(config->engineOrProviderExplicit);
}

TEST(Config, HelpFlagShortCircuitsParsing)
{
    std::string error;
    // Everything after --help is ignored, even an otherwise-invalid flag.
    const char* argvHelp[] = {"transcriber", "--help", "--bogus"};
    auto config = parseArgs(3, argvHelp, error);
    ASSERT_TRUE(config.has_value());
    EXPECT_TRUE(config->showHelp);
    const char* argvShort[] = {"transcriber", "-h"};
    auto configShort = parseArgs(2, argvShort, error);
    ASSERT_TRUE(configShort.has_value());
    EXPECT_TRUE(configShort->showHelp);
}

TEST(Config, UnknownFlagErrorPointsToHelp)
{
    std::string error;
    const char* argvBad[] = {"transcriber", "--bogus"};
    EXPECT_FALSE(parseArgs(2, argvBad, error).has_value());
    EXPECT_NE(error.find("--help"), std::string::npos);
}

TEST(Config, UsageTextNamesEveryFlag)
{
    const std::string usage = usageText();
    for (const char* flag : {"--engine", "--provider", "--port", "--model-dir", "--decoding",
                             "--endpoint-silence", "--headless", "--duration", "--help"})
    {
        EXPECT_NE(usage.find(flag), std::string::npos) << flag;
    }
}

TEST(Config, SettingsFileRoundTrip)
{
    Config out;
    out.engine = "deepgram";
    out.provider = "cuda";
    out.deepgramKey = "abc123testkey";
    out.askOnStartup = true;
    const std::string path = std::string(::testing::TempDir()) + "settings-roundtrip.json";
    ASSERT_TRUE(saveSettingsFile(path, out));
    Config in;
    ASSERT_TRUE(loadSettingsFile(path, in));
    EXPECT_EQ(in.engine, "deepgram");
    EXPECT_EQ(in.provider, "cuda");
    EXPECT_EQ(in.deepgramKey, "abc123testkey");
    EXPECT_TRUE(in.askOnStartup);
    // The key must be stored DPAPI-protected: the file on disk must never
    // contain the plaintext key.
    std::ifstream savedFile(path);
    const std::string savedText((std::istreambuf_iterator<char>(savedFile)),
                                std::istreambuf_iterator<char>());
    EXPECT_EQ(savedText.find("abc123testkey"), std::string::npos);
    EXPECT_NE(savedText.find("deepgramKeyProtected"), std::string::npos);
}

TEST(Config, SettingsFileLegacyPlaintextKeyMigrates)
{
    // Settings written before encryption-at-rest stored the key in plaintext;
    // loading must still honor it (the next save upgrades the file).
    const std::string path = std::string(::testing::TempDir()) + "settings-legacy.json";
    std::ofstream(path) << R"({"engine":"deepgram","provider":"cpu","deepgramKey":"legacykey"})";
    Config in;
    ASSERT_TRUE(loadSettingsFile(path, in));
    EXPECT_EQ(in.deepgramKey, "legacykey");
}

TEST(Config, SettingsFileMissingLeavesDefaults)
{
    Config in;
    EXPECT_FALSE(loadSettingsFile("Z:/definitely/missing/settings.json", in));
    EXPECT_EQ(in.engine, "deepgram");
    EXPECT_EQ(in.provider, "cpu");
}

TEST(Config, SettingsFileInvalidValuesIgnored)
{
    const std::string path = std::string(::testing::TempDir()) + "settings-bad.json";
    std::ofstream(path) << R"({"engine":"whisper","provider":"cuda"})";
    Config in;
    EXPECT_TRUE(loadSettingsFile(path, in));
    EXPECT_EQ(in.engine, "deepgram");  // Invalid engine value fell back to default.
    EXPECT_EQ(in.provider, "cuda");    // Valid provider still applied.
}

TEST(Config, CliOverridesSettingsFileBase)
{
    Config base;
    base.engine = "deepgram";
    base.provider = "cuda";
    std::vector<const char*> args{"transcriber.exe", "--engine", "sherpa"};
    std::string err;
    auto config = parseArgs(static_cast<int>(args.size()), args.data(), err, base);
    ASSERT_TRUE(config) << err;
    EXPECT_EQ(config->engine, "sherpa");  // Explicit CLI flag wins over the file.
    EXPECT_EQ(config->provider, "cuda");  // Untouched file value survives.
    EXPECT_TRUE(config->engineOrProviderExplicit);
}

TEST(Config, ParsesAllFlags)
{
    std::string err;
    auto config = parse(
        {"--engine", "deepgram", "--provider", "cuda", "--port", "9000", "--model-dir", "D:/models",
         "--decoding", "greedy", "--endpoint-silence", "0.5", "--headless", "--duration", "12.5"},
        err);
    ASSERT_TRUE(config) << err;
    EXPECT_EQ(config->engine, "deepgram");
    EXPECT_EQ(config->provider, "cuda");
    EXPECT_EQ(config->port, 9000);
    EXPECT_EQ(config->modelDir, "D:/models");
    EXPECT_EQ(config->decoding, "greedy");
    EXPECT_DOUBLE_EQ(config->endpointSilenceSec, 0.5);
    EXPECT_TRUE(config->headless);
    EXPECT_DOUBLE_EQ(config->durationSec, 12.5);
}

TEST(Config, RejectsBadValues)
{
    std::string err;
    EXPECT_FALSE(parse({"--engine", "whisper"}, err));
    EXPECT_TRUE(parse({"--engine", "parakeet"}, err));
    EXPECT_FALSE(parse({"--provider", "opencl"}, err));
    EXPECT_FALSE(parse({"--decoding", "fast"}, err));
    EXPECT_FALSE(parse({"--endpoint-silence", "abc"}, err));
    EXPECT_FALSE(parse({"--endpoint-silence", "0.1"}, err));  // Below range.
    EXPECT_FALSE(parse({"--endpoint-silence", "6"}, err));    // Above range.
    EXPECT_FALSE(parse({"--port", "notanumber"}, err));
    EXPECT_FALSE(parse({"--port"}, err));  // Missing value.
    EXPECT_FALSE(parse({"--unknown-flag"}, err));
}

TEST(Config, RejectsOutOfRangePorts)
{
    std::string err;
    EXPECT_FALSE(parse({"--port", "0"}, err));
    EXPECT_FALSE(parse({"--port", "-1"}, err));
    EXPECT_FALSE(parse({"--port", "65536"}, err));
    EXPECT_TRUE(parse({"--port", "65535"}, err));  // Boundary: valid.
    EXPECT_TRUE(parse({"--port", "1"}, err));      // Boundary: valid.
}

TEST(Config, RejectsPartialNumericPort)
{
    std::string err;
    EXPECT_FALSE(parse({"--port", "8080x"}, err));  // Trailing garbage: full-string parse branch.
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
    auto config = parse({"--duration", "12abc"}, err);
    EXPECT_TRUE(config);
    EXPECT_DOUBLE_EQ(config->durationSec, 12.0);
}

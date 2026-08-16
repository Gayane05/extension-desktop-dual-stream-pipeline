// desktop/src/app/config.cpp
//
// Hand-rolled argv parser for Config (see config.h): flag-by-flag validation
// with immediate std::nullopt + error message on the first bad/missing
// value, rather than a generic options library, since the flag set is small
// and fixed. Called once from main() before any engine/pipeline is created.
#include "app/config.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <charconv>
#include <cstdio>
#include <string_view>

#include "core/secret_store.h"

namespace dsp
{

// Highest valid TCP port number accepted by --port.
inline constexpr int kMaxPort = 65535;
// Valid range (seconds) for --endpoint-silence.
inline constexpr double kMinEndpointSilenceSec = 0.2;
inline constexpr double kMaxEndpointSilenceSec = 5.0;
// Read chunk size when slurping settings.json off disk.
inline constexpr size_t kSettingsReadBufferSize = 512;

static bool takeValue(int argc, const char* const* argv, int& i, std::string& out,
                      std::string& error)
{
    if (i + 1 >= argc)
    {
        error = std::string(argv[i]) + " requires a value";
        return false;
    }
    out = argv[++i];
    return true;
}

std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error)
{
    return parseArgs(argc, argv, error, Config{});
}

std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error,
                                const Config& base)
{
    Config config = base;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view flag = argv[i];
        std::string flagValue;
        if (flag == "--engine")
        {
            if (!takeValue(argc, argv, i, flagValue, error))
            {
                return std::nullopt;
            }
            if (flagValue != "sherpa" && flagValue != "deepgram" && flagValue != "parakeet")
            {
                error = "engine must be sherpa|deepgram|parakeet";
                return std::nullopt;
            }
            config.engine = flagValue;
            config.engineOrProviderExplicit = true;
        }
        else if (flag == "--provider")
        {
            if (!takeValue(argc, argv, i, flagValue, error))
            {
                return std::nullopt;
            }
            if (flagValue != "cpu" && flagValue != "cuda" && flagValue != "tensorrt")
            {
                error = "provider must be cpu|cuda|tensorrt";
                return std::nullopt;
            }
            config.provider = flagValue;
            config.engineOrProviderExplicit = true;
        }
        else if (flag == "--port")
        {
            if (!takeValue(argc, argv, i, flagValue, error))
            {
                return std::nullopt;
            }
            auto portParseResult =
                std::from_chars(flagValue.data(), flagValue.data() + flagValue.size(), config.port);
            if (portParseResult.ec != std::errc{} ||
                portParseResult.ptr != flagValue.data() + flagValue.size() || config.port <= 0 ||
                config.port > kMaxPort)
            {
                error = "invalid port";
                return std::nullopt;
            }
        }
        else if (flag == "--model-dir")
        {
            if (!takeValue(argc, argv, i, flagValue, error))
            {
                return std::nullopt;
            }
            config.modelDir = flagValue;
        }
        else if (flag == "--decoding")
        {
            if (!takeValue(argc, argv, i, flagValue, error))
            {
                return std::nullopt;
            }
            if (flagValue != "beam" && flagValue != "greedy")
            {
                error = "decoding must be beam|greedy";
                return std::nullopt;
            }
            config.decoding = flagValue;
        }
        else if (flag == "--endpoint-silence")
        {
            if (!takeValue(argc, argv, i, flagValue, error))
            {
                return std::nullopt;
            }
            try
            {
                config.endpointSilenceSec = std::stod(flagValue);
            }
            catch (...)
            {
                error = "invalid endpoint-silence";
                return std::nullopt;
            }
            if (config.endpointSilenceSec < kMinEndpointSilenceSec ||
                config.endpointSilenceSec > kMaxEndpointSilenceSec)
            {
                error = "endpoint-silence must be 0.2..5.0 seconds";
                return std::nullopt;
            }
        }
        else if (flag == "--headless")
        {
            config.headless = true;
        }
        else if (flag == "--duration")
        {
            if (!takeValue(argc, argv, i, flagValue, error))
            {
                return std::nullopt;
            }
            try
            {
                config.durationSec = std::stod(flagValue);
            }
            catch (...)
            {
                error = "invalid duration";
                return std::nullopt;
            }
        }
        else
        {
            error = "unknown flag: " + std::string(flag);
            return std::nullopt;
        }
    }
    return config;
}

// Only engine + provider are persisted: they are the "how do you want to
// run" choice the first-run chooser asks about. Everything else stays a CLI
// concern so scripted runs (E2E, headless) remain fully self-describing.
bool loadSettingsFile(const std::string& path, Config& into)
{
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || !file)
    {
        return false;
    }
    std::string text;
    char readBuffer[kSettingsReadBufferSize];
    size_t bytesRead = 0;
    while ((bytesRead = fread(readBuffer, 1, sizeof(readBuffer), file)) > 0)
    {
        text.append(readBuffer, bytesRead);
    }
    fclose(file);

    rapidjson::Document jsonDoc;
    jsonDoc.Parse(text.c_str());
    if (jsonDoc.HasParseError() || !jsonDoc.IsObject())
    {
        return false;
    }
    // Validate against the same value sets parseArgs enforces; a hand-edited
    // bad value falls back to the current (default) setting rather than
    // smuggling an invalid string past CLI validation.
    if (jsonDoc.HasMember("engine") && jsonDoc["engine"].IsString())
    {
        const std::string value = jsonDoc["engine"].GetString();
        if (value == "sherpa" || value == "deepgram" || value == "parakeet")
        {
            into.engine = value;
        }
    }
    if (jsonDoc.HasMember("provider") && jsonDoc["provider"].IsString())
    {
        const std::string value = jsonDoc["provider"].GetString();
        if (value == "cpu" || value == "cuda" || value == "tensorrt")
        {
            into.provider = value;
        }
    }
    // The key is stored DPAPI-protected (see core/secret_store.h). A blob
    // that fails to unprotect (different user, re-imaged machine, corruption)
    // is treated as "no key stored" -- the user simply re-enters it.
    if (jsonDoc.HasMember("deepgramKeyProtected") && jsonDoc["deepgramKeyProtected"].IsString())
    {
        if (auto plainKey = unprotectSecret(jsonDoc["deepgramKeyProtected"].GetString()))
        {
            into.deepgramKey = *plainKey;
        }
    }
    // Legacy migration: settings written before encryption-at-rest stored the
    // key in plaintext. Honor it on load; the next save rewrites the file
    // with the protected form only.
    else if (jsonDoc.HasMember("deepgramKey") && jsonDoc["deepgramKey"].IsString())
    {
        into.deepgramKey = jsonDoc["deepgramKey"].GetString();
    }
    return true;
}

bool saveSettingsFile(const std::string& path, const Config& cfg)
{
    rapidjson::StringBuffer jsonBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> jsonWriter(jsonBuffer);
    jsonWriter.StartObject();
    jsonWriter.Key("engine");
    jsonWriter.String(cfg.engine.c_str());
    jsonWriter.Key("provider");
    jsonWriter.String(cfg.provider.c_str());
    if (!cfg.deepgramKey.empty())
    {
        // Never write the key in plaintext. If protection fails (it should
        // not on a healthy Windows), the key is simply omitted -- losing
        // persistence is safer than persisting it readable.
        if (auto protectedBlob = protectSecret(cfg.deepgramKey))
        {
            jsonWriter.Key("deepgramKeyProtected");
            jsonWriter.String(protectedBlob->c_str());
        }
        else
        {
            std::fprintf(stderr, "warning: could not protect the API key; not persisting it\n");
        }
    }
    jsonWriter.EndObject();

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
    {
        return false;
    }
    const size_t len = jsonBuffer.GetSize();
    const bool ok = fwrite(jsonBuffer.GetString(), 1, len, file) == len;
    return fclose(file) == 0 && ok;
}

}  // namespace dsp

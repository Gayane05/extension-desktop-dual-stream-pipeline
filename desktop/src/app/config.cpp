// desktop/src/app/config.cpp
//
// Hand-rolled argv parser for Config (see config.h): flag-by-flag validation
// with immediate std::nullopt + error message on the first bad/missing
// value, rather than a generic options library, since the flag set is small
// and fixed. Called once from main() before any engine/pipeline is created.
#include "app/config.h"

#include <charconv>
#include <cstdio>
#include <string_view>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace dsp {

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
    Config c = base;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        std::string v;
        if (a == "--engine")
        {
            if (!takeValue(argc, argv, i, v, error))
            {
                return std::nullopt;
            }
            if (v != "sherpa" && v != "deepgram")
            {
                error = "engine must be sherpa|deepgram";
                return std::nullopt;
            }
            c.engine = v;
            c.engineOrProviderExplicit = true;
        }
        else if (a == "--provider")
        {
            if (!takeValue(argc, argv, i, v, error))
            {
                return std::nullopt;
            }
            if (v != "cpu" && v != "cuda" && v != "tensorrt")
            {
                error = "provider must be cpu|cuda|tensorrt";
                return std::nullopt;
            }
            c.provider = v;
            c.engineOrProviderExplicit = true;
        }
        else if (a == "--port")
        {
            if (!takeValue(argc, argv, i, v, error))
            {
                return std::nullopt;
            }
            auto res = std::from_chars(v.data(), v.data() + v.size(), c.port);
            if (res.ec != std::errc{} || res.ptr != v.data() + v.size() || c.port <= 0 ||
                c.port > 65535)
            {
                error = "invalid port";
                return std::nullopt;
            }
        }
        else if (a == "--model-dir")
        {
            if (!takeValue(argc, argv, i, v, error))
            {
                return std::nullopt;
            }
            c.modelDir = v;
        }
        else if (a == "--decoding")
        {
            if (!takeValue(argc, argv, i, v, error))
            {
                return std::nullopt;
            }
            if (v != "beam" && v != "greedy")
            {
                error = "decoding must be beam|greedy";
                return std::nullopt;
            }
            c.decoding = v;
        }
        else if (a == "--endpoint-silence")
        {
            if (!takeValue(argc, argv, i, v, error))
            {
                return std::nullopt;
            }
            try
            {
                c.endpointSilenceSec = std::stod(v);
            }
            catch (...)
            {
                error = "invalid endpoint-silence";
                return std::nullopt;
            }
            if (c.endpointSilenceSec < 0.2 || c.endpointSilenceSec > 5.0)
            {
                error = "endpoint-silence must be 0.2..5.0 seconds";
                return std::nullopt;
            }
        }
        else if (a == "--headless")
        {
            c.headless = true;
        }
        else if (a == "--duration")
        {
            if (!takeValue(argc, argv, i, v, error))
            {
                return std::nullopt;
            }
            try
            {
                c.durationSec = std::stod(v);
            }
            catch (...)
            {
                error = "invalid duration";
                return std::nullopt;
            }
        }
        else
        {
            error = "unknown flag: " + std::string(a);
            return std::nullopt;
        }
    }
    return c;
}

// Only engine + provider are persisted: they are the "how do you want to
// run" choice the first-run chooser asks about. Everything else stays a CLI
// concern so scripted runs (E2E, headless) remain fully self-describing.
bool loadSettingsFile(const std::string& path, Config& into)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
    {
        return false;
    }
    std::string text;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        text.append(buf, n);
    }
    fclose(f);

    rapidjson::Document d;
    d.Parse(text.c_str());
    if (d.HasParseError() || !d.IsObject())
    {
        return false;
    }
    // Validate against the same value sets parseArgs enforces; a hand-edited
    // bad value falls back to the current (default) setting rather than
    // smuggling an invalid string past CLI validation.
    if (d.HasMember("engine") && d["engine"].IsString())
    {
        const std::string v = d["engine"].GetString();
        if (v == "sherpa" || v == "deepgram")
        {
            into.engine = v;
        }
    }
    if (d.HasMember("provider") && d["provider"].IsString())
    {
        const std::string v = d["provider"].GetString();
        if (v == "cpu" || v == "cuda" || v == "tensorrt")
        {
            into.provider = v;
        }
    }
    if (d.HasMember("deepgramKey") && d["deepgramKey"].IsString())
    {
        into.deepgramKey = d["deepgramKey"].GetString();
    }
    return true;
}

bool saveSettingsFile(const std::string& path, const Config& cfg)
{
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("engine");
    w.String(cfg.engine.c_str());
    w.Key("provider");
    w.String(cfg.provider.c_str());
    if (!cfg.deepgramKey.empty())
    {
        w.Key("deepgramKey");
        w.String(cfg.deepgramKey.c_str());
    }
    w.EndObject();

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
    {
        return false;
    }
    const size_t len = sb.GetSize();
    const bool ok = fwrite(sb.GetString(), 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

}  // namespace dsp

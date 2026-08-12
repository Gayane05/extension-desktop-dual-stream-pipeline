#include "app/config.h"

#include <charconv>
#include <cstring>
#include <string_view>

namespace dsp {

static bool takeValue(int argc, const char* const* argv, int& i, std::string& out,
                      std::string& error) {
    if (i + 1 >= argc) { error = std::string(argv[i]) + " requires a value"; return false; }
    out = argv[++i];
    return true;
}

std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        std::string v;
        if (a == "--engine") {
            if (!takeValue(argc, argv, i, v, error)) return std::nullopt;
            if (v != "sherpa" && v != "deepgram") { error = "engine must be sherpa|deepgram"; return std::nullopt; }
            c.engine = v;
        } else if (a == "--provider") {
            if (!takeValue(argc, argv, i, v, error)) return std::nullopt;
            if (v != "cpu" && v != "cuda" && v != "tensorrt") { error = "provider must be cpu|cuda|tensorrt"; return std::nullopt; }
            c.provider = v;
        } else if (a == "--port") {
            if (!takeValue(argc, argv, i, v, error)) return std::nullopt;
            auto res = std::from_chars(v.data(), v.data() + v.size(), c.port);
            if (res.ec != std::errc{} || res.ptr != v.data() + v.size() || c.port <= 0 || c.port > 65535) {
                error = "invalid port"; return std::nullopt;
            }
        } else if (a == "--model-dir") {
            if (!takeValue(argc, argv, i, v, error)) return std::nullopt;
            c.modelDir = v;
        } else if (a == "--headless") {
            c.headless = true;
        } else if (a == "--duration") {
            if (!takeValue(argc, argv, i, v, error)) return std::nullopt;
            try { c.durationSec = std::stod(v); } catch (...) { error = "invalid duration"; return std::nullopt; }
        } else {
            error = "unknown flag: " + std::string(a);
            return std::nullopt;
        }
    }
    return c;
}

}  // namespace dsp

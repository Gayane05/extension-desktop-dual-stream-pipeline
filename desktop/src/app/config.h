#pragma once
#include <optional>
#include <string>

namespace dsp {
struct Config {
    std::string engine = "sherpa";
    std::string provider = "cpu";
    int port = 8765;
    std::string modelDir = "models";
    bool headless = false;
    double durationSec = 0;
};
std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error);
}  // namespace dsp

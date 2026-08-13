#pragma once
#include <optional>
#include <string>

namespace dsp {
struct Config {
    std::string engine = "sherpa";
    std::string provider = "cpu";
    int port = 8765;
    std::string modelDir = "models";
    std::string decoding = "beam";  // beam (modified_beam_search) | greedy
    bool headless = false;
    double durationSec = 0;
};
std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error);
}  // namespace dsp

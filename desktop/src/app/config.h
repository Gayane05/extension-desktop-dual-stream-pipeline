// desktop/src/app/config.h
//
// Process-wide settings parsed from CLI args (see config.cpp) and threaded
// through to main.cpp's engine/pipeline construction and (when --headless)
// the timed run loop. Plain data + one free function; no behavior lives here.
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
    // Trailing-silence (seconds) after speech before an utterance is
    // finalized (sherpa endpoint rule2). Smaller = more sentence-like splits.
    double endpointSilenceSec = 0.8;
    bool headless = false;
    double durationSec = 0;
};
std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error);
}  // namespace dsp

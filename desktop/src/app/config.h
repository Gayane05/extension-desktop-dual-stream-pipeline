// desktop/src/app/config.h
//
// Process-wide settings, resolved in three layers (weakest to strongest):
// built-in defaults -> settings.json next to the exe (written by the GUI's
// first-run chooser / Settings screen) -> explicit CLI flags. Threaded
// through to main.cpp's engine/pipeline construction and (when --headless)
// the timed run loop. Plain data + free functions; no behavior lives here.
#pragma once
#include <optional>
#include <string>

namespace dsp {
struct Config {
    std::string engine = "deepgram";
    std::string provider = "cpu";
    int port = 8765;
    std::string modelDir = "models";
    std::string decoding = "beam";  // beam (modified_beam_search) | greedy
    // Trailing-silence (seconds) after speech before an utterance is
    // finalized (sherpa endpoint rule2). Smaller = more sentence-like splits.
    double endpointSilenceSec = 0.8;
    bool headless = false;
    double durationSec = 0;
    // Deepgram API key entered in the Settings screen. Persisted with the
    // settings (local, gitignored); when empty, the DEEPGRAM_API_KEY
    // environment variable is used instead (see main.cpp's makeEngine).
    std::string deepgramKey;
    // True when --engine or --provider was given on the command line. Not
    // persisted; kept so callers can distinguish an explicit CLI choice from
    // defaults/settings-file values.
    bool engineOrProviderExplicit = false;
};

std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error);
// Same, but starts from `base` instead of built-in defaults, so values loaded
// from the settings file survive unless a CLI flag overrides them.
std::optional<Config> parseArgs(int argc, const char* const* argv, std::string& error,
                                const Config& base);

// settings.json persistence (engine + provider only -- everything else stays
// CLI-driven). load returns false and leaves `into` untouched when the file
// is absent or unreadable; save overwrites atomically enough for our use
// (single local writer). Both take the full file path.
bool loadSettingsFile(const std::string& path, Config& into);
bool saveSettingsFile(const std::string& path, const Config& cfg);

// runUi() exit code asking main() to reopen the mode chooser and rebuild the
// engine with the newly saved settings (the toolbar's Settings button).
inline constexpr int kRunUiRestartSetup = 2;
}  // namespace dsp

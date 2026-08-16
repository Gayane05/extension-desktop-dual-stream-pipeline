// desktop/src/main.cpp
//
// Process entry point: parses Config, constructs the chosen ISttEngine
// (sherpa or deepgram), wires it into a Pipeline (which owns the WsServer
// the extension connects to), and then either runs the ImGui window
// (runUi(), in main_window.cpp) or, under --headless, a timed loop that
// prints TranscriptEvents as JSONL to stdout/stderr for scripted/E2E use.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "app/config.h"
#include "app/pipeline.h"
#include "app/transcript_model.h"
#include "stt/deepgram_engine.h"
#include "stt/parakeet_engine.h"
#include "stt/sherpa_engine.h"

namespace dsp
{
// Both implemented in desktop/src/ui/main_window.cpp. runUi returns
// kRunUiRestartSetup when the Settings button asks for the mode chooser.
int runUi(Pipeline& pipeline, TranscriptModel& model, ISttEngine& engine, const Config& cfg);
bool runSetupUi(Config& cfg);
}  // namespace dsp

static std::atomic<bool> g_stop{false};
static void onSignal(int)
{
    g_stop = true;
}

// How many parent directories resolveDefaultModelDir() walks up from the exe
// looking for a "models" dir (build/Release -> build -> desktop).
constexpr int kModelDirSearchLevels = 2;
// Headless run-loop tick period.
constexpr int kHeadlessTickMs = 100;
// Push a status heartbeat every this-many ticks (~1x/second at
// kHeadlessTickMs).
constexpr int kStatusPushEveryTicks = 10;

// Makes the default model path work for double-click launches. The README's
// documented flow runs the exe from desktop/, where the default "models" dir
// resolves directly -- but Explorer launches set cwd to the exe's own folder
// (desktop/build/Release), where it does not. When the user did NOT override
// --model-dir and the default is missing at the cwd, look next to the exe and
// up to two parent levels (build/Release -> build -> desktop). An explicitly
// passed --model-dir is never second-guessed.
static void resolveDefaultModelDir(dsp::Config& cfg)
{
    namespace fs = std::filesystem;
    if (cfg.modelDir != "models" || fs::exists(cfg.modelDir))
    {
        return;
    }
#ifdef _WIN32
    char exePath[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        return;
    }
    fs::path dir = fs::path(exePath).parent_path();
    for (int up = 0; up <= kModelDirSearchLevels; ++up)
    {
        const fs::path candidate = dir / "models";
        if (fs::exists(candidate))
        {
            cfg.modelDir = candidate.string();
            return;
        }
        dir = dir.parent_path();
    }
#endif
}

static std::unique_ptr<dsp::ISttEngine> makeEngine(const dsp::Config& cfg,
                                                   dsp::TranscriptCallback transcriptCallback)
{
    dsp::EngineOptions opts;
    opts.modelDir = cfg.modelDir;
    opts.provider = cfg.provider;
    opts.decoding = cfg.decoding;
    opts.endpointSilenceSec = cfg.endpointSilenceSec;
    opts.language = cfg.language;
    // Key precedence: one entered in the Settings screen (persisted with the
    // saved settings) wins; the DEEPGRAM_API_KEY environment variable is the
    // fallback so scripted/headless runs keep working without any UI.
    if (!cfg.deepgramKey.empty())
    {
        opts.deepgramKey = cfg.deepgramKey;
    }
    else if (const char* envApiKey = std::getenv("DEEPGRAM_API_KEY"))
    {
        opts.deepgramKey = envApiKey;
    }
    if (cfg.engine == "deepgram")
    {
        return std::make_unique<dsp::DeepgramEngine>(opts, std::move(transcriptCallback));
    }
    if (cfg.engine == "parakeet")
    {
        return std::make_unique<dsp::ParakeetEngine>(opts, std::move(transcriptCallback));
    }
    return std::make_unique<dsp::SherpaEngine>(opts, std::move(transcriptCallback));
}

// Deviation from the task brief: the brief's sketch built the headless JSONL
// line with fprintf("...\"text\":\"%s\"...", transcriptEvent.text.c_str()), which produces
// invalid JSON whenever the transcript text contains a quote or backslash (or
// other characters requiring escaping). Emit the line with RapidJSON's
// Writer instead -- the same approach protocol.cpp already uses for
// buildStatusJson/buildErrorJson -- so the text field is always valid JSON.
static std::string buildTranscriptLine(const dsp::TranscriptEvent& transcriptEvent)
{
    rapidjson::StringBuffer jsonBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> jsonWriter(jsonBuffer);
    jsonWriter.StartObject();
    jsonWriter.Key("stream");
    jsonWriter.String(dsp::streamName(transcriptEvent.stream));
    jsonWriter.Key("final");
    jsonWriter.Bool(transcriptEvent.isFinal);
    jsonWriter.Key("ts");
    jsonWriter.Double(transcriptEvent.tsMs);
    // Use the (data, length) overload rather than c_str(): text containing an
    // embedded NUL would otherwise be silently truncated at the first one.
    jsonWriter.Key("text");
    jsonWriter.String(transcriptEvent.text.data(),
                      static_cast<rapidjson::SizeType>(transcriptEvent.text.size()));
    jsonWriter.EndObject();
    return jsonBuffer.GetString();
}

// settings.json lives next to the exe (portable-app pattern) so double-click
// and command-line launches share the same persisted choice regardless of
// cwd. Falls back to a cwd-relative name if the exe path cannot be resolved.
static std::string settingsFilePath()
{
#ifdef _WIN32
    char exePath[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0)
    {
        namespace fs = std::filesystem;
        return (fs::path(exePath).parent_path() / "settings.json").string();
    }
#endif
    return "settings.json";
}

int main(int argc, char** argv)
{
    std::string err;
    // Config resolution order: defaults -> settings.json -> CLI flags.
    dsp::Config base;
    const std::string settingsPath = settingsFilePath();
    const bool haveSettings = dsp::loadSettingsFile(settingsPath, base);
    auto cfg = dsp::parseArgs(argc, argv, err, base);
    if (!cfg)
    {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    if (cfg->showHelp)
    {
        std::fprintf(stdout, "%s", dsp::usageText().c_str());
        return 0;
    }

    resolveDefaultModelDir(*cfg);

    // The Settings page opens on the very first run (no settings file yet)
    // and, if the user checked "Ask every startup", on every launch. It also
    // reopens from the Settings button and when the chosen engine fails to
    // start in GUI mode (recovery path). Explicit --engine/--provider flags
    // and headless runs skip it: they already state how to run.
    bool showSetup =
        !cfg->headless && !cfg->engineOrProviderExplicit && (!haveSettings || cfg->askOnStartup);

    // Each loop iteration is one full engine lifetime. The Settings button
    // exits runUi with kRunUiRestartSetup; we then re-run the chooser, persist
    // the new choice, and rebuild engine + pipeline from scratch.
    for (;;)
    {
        if (showSetup)
        {
            if (!dsp::runSetupUi(*cfg))
            {
                return 0;  // Chooser closed without choosing = quit.
            }
            if (!dsp::saveSettingsFile(settingsPath, *cfg))
            {
                std::fprintf(stderr, "warning: could not write %s\n", settingsPath.c_str());
            }
            showSetup = false;
        }

        dsp::TranscriptModel model;
        auto engine = makeEngine(*cfg, [&](const dsp::TranscriptEvent& transcriptEvent) {
            model.apply(transcriptEvent);
            if (cfg->headless)
            {
                FILE* out = transcriptEvent.isFinal ? stdout : stderr;
                std::fprintf(out, "%s\n", buildTranscriptLine(transcriptEvent).c_str());
                std::fflush(out);
            }
        });
        if (!engine->start(err))
        {
            std::fprintf(stderr, "engine error: %s\n", err.c_str());
#ifdef _WIN32
            // Console-only output is easy to miss when the app was launched by
            // double-click (no attached console). Surface the exact error (e.g.
            // the missing-model download command) in a message box too, but only
            // when we'd otherwise have opened a window -- headless runs (CI, the
            // E2E script) must stay console-only.
            if (!cfg->headless)
            {
                MessageBoxA(nullptr, err.c_str(),
                            "Dual-Stream Transcriber - engine failed to start",
                            MB_OK | MB_ICONERROR);
            }
#endif
            if (!cfg->headless)
            {
                // Give the user a way out (e.g. picked Deepgram without a key):
                // reopen the chooser instead of dying.
                err.clear();
                showSetup = true;
                continue;
            }
            return 3;
        }

        dsp::Pipeline pipeline(*cfg, *engine);
        if (!pipeline.start(err))
        {
            std::fprintf(stderr, "server error: %s\n", err.c_str());
            return 4;
        }
        std::fprintf(stderr, "listening on ws://127.0.0.1:%d (engine=%s provider=%s)\n", cfg->port,
                     engine->name().c_str(), engine->effectiveProvider().c_str());

        if (cfg->headless)
        {
            std::signal(SIGINT, onSignal);
            auto deadline =
                std::chrono::steady_clock::now() + std::chrono::duration<double>(cfg->durationSec);
            // Status otherwise only pushes on hello/clientGone, which leaves a
            // long-lived headless client's view stale for the whole run. Push
            // ~1x/second (every 10th 100ms tick) so a connected client's status
            // (e.g. dropped-chunk counters, stream state) stays current. This
            // goes to WS clients only (WsServer::broadcast), never to stdout, so
            // it cannot corrupt the JSONL transcript stream the E2E script reads.
            int tick = 0;
            while (!g_stop &&
                   (cfg->durationSec <= 0 || std::chrono::steady_clock::now() < deadline))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(kHeadlessTickMs));
                if (++tick % kStatusPushEveryTicks == 0)
                {
                    pipeline.pushStatus();
                }
            }
            pipeline.stop();
            engine->stop();
            return 0;
        }

        const int uiExitCode = dsp::runUi(pipeline, model, *engine, *cfg);
        pipeline.stop();
        engine->stop();
        if (uiExitCode == dsp::kRunUiRestartSetup)
        {
            showSetup = true;
            continue;
        }
        return uiExitCode;
    }
}

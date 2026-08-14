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
#include "stt/sherpa_engine.h"

namespace dsp {
int runUi(Pipeline& pipeline, TranscriptModel& model, ISttEngine& engine,
          const Config& cfg);  // implemented in desktop/src/ui/main_window.cpp
}

static std::atomic<bool> g_stop{false};
static void onSignal(int)
{
    g_stop = true;
}

static std::unique_ptr<dsp::ISttEngine> makeEngine(const dsp::Config& cfg,
                                                   dsp::TranscriptCallback cb)
{
    dsp::EngineOptions opts;
    opts.modelDir = cfg.modelDir;
    opts.provider = cfg.provider;
    opts.decoding = cfg.decoding;
    opts.endpointSilenceSec = cfg.endpointSilenceSec;
    if (const char* k = std::getenv("DEEPGRAM_API_KEY"))
    {
        opts.deepgramKey = k;
    }
    if (cfg.engine == "deepgram")
    {
        return std::make_unique<dsp::DeepgramEngine>(opts, std::move(cb));
    }
    return std::make_unique<dsp::SherpaEngine>(opts, std::move(cb));
}

// Deviation from the task brief: the brief's sketch built the headless JSONL
// line with fprintf("...\"text\":\"%s\"...", ev.text.c_str()), which produces
// invalid JSON whenever the transcript text contains a quote or backslash (or
// other characters requiring escaping). Emit the line with RapidJSON's
// Writer instead -- the same approach protocol.cpp already uses for
// buildStatusJson/buildErrorJson -- so the text field is always valid JSON.
static std::string buildTranscriptLine(const dsp::TranscriptEvent& ev)
{
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("stream");
    w.String(dsp::streamName(ev.stream));
    w.Key("final");
    w.Bool(ev.isFinal);
    w.Key("ts");
    w.Double(ev.tsMs);
    // Use the (data, length) overload rather than c_str(): text containing an
    // embedded NUL would otherwise be silently truncated at the first one.
    w.Key("text");
    w.String(ev.text.data(), static_cast<rapidjson::SizeType>(ev.text.size()));
    w.EndObject();
    return sb.GetString();
}

int main(int argc, char** argv)
{
    std::string err;
    auto cfg = dsp::parseArgs(argc, argv, err);
    if (!cfg)
    {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }

    dsp::TranscriptModel model;
    auto engine = makeEngine(*cfg, [&](const dsp::TranscriptEvent& ev) {
        model.apply(ev);
        if (cfg->headless)
        {
            FILE* out = ev.isFinal ? stdout : stderr;
            std::fprintf(out, "%s\n", buildTranscriptLine(ev).c_str());
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
            MessageBoxA(nullptr, err.c_str(), "Dual-Stream Transcriber - engine failed to start",
                        MB_OK | MB_ICONERROR);
        }
#endif
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
        while (!g_stop && (cfg->durationSec <= 0 || std::chrono::steady_clock::now() < deadline))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (++tick % 10 == 0)
            {
                pipeline.pushStatus();
            }
        }
        pipeline.stop();
        engine->stop();
        return 0;
    }
    int rc = dsp::runUi(pipeline, model, *engine, *cfg);
    pipeline.stop();
    engine->stop();
    return rc;
}

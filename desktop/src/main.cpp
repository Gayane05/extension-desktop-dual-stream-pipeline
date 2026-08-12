#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

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
static void onSignal(int) { g_stop = true; }

static std::unique_ptr<dsp::ISttEngine> makeEngine(const dsp::Config& cfg,
                                                   dsp::TranscriptCallback cb) {
    dsp::EngineOptions opts;
    opts.modelDir = cfg.modelDir;
    opts.provider = cfg.provider;
    if (const char* k = std::getenv("DEEPGRAM_API_KEY")) opts.deepgramKey = k;
    if (cfg.engine == "deepgram")
        return std::make_unique<dsp::DeepgramEngine>(opts, std::move(cb));
    return std::make_unique<dsp::SherpaEngine>(opts, std::move(cb));
}

// Deviation from the task brief: the brief's sketch built the headless JSONL
// line with fprintf("...\"text\":\"%s\"...", ev.text.c_str()), which produces
// invalid JSON whenever the transcript text contains a quote or backslash (or
// other characters requiring escaping). Emit the line with RapidJSON's
// Writer instead -- the same approach protocol.cpp already uses for
// buildStatusJson/buildErrorJson -- so the text field is always valid JSON.
static std::string buildTranscriptLine(const dsp::TranscriptEvent& ev) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("stream"); w.String(dsp::streamName(ev.stream));
    w.Key("final"); w.Bool(ev.isFinal);
    w.Key("ts"); w.Double(ev.tsMs);
    // Use the (data, length) overload rather than c_str(): text containing an
    // embedded NUL would otherwise be silently truncated at the first one.
    w.Key("text"); w.String(ev.text.data(), static_cast<rapidjson::SizeType>(ev.text.size()));
    w.EndObject();
    return sb.GetString();
}

int main(int argc, char** argv) {
    std::string err;
    auto cfg = dsp::parseArgs(argc, argv, err);
    if (!cfg) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }

    dsp::TranscriptModel model;
    auto engine = makeEngine(*cfg, [&](const dsp::TranscriptEvent& ev) {
        model.apply(ev);
        if (cfg->headless) {
            FILE* out = ev.isFinal ? stdout : stderr;
            std::fprintf(out, "%s\n", buildTranscriptLine(ev).c_str());
            std::fflush(out);
        }
    });
    if (!engine->start(err)) { std::fprintf(stderr, "engine error: %s\n", err.c_str()); return 3; }

    dsp::Pipeline pipeline(*cfg, *engine, model);
    if (!pipeline.start(err)) { std::fprintf(stderr, "server error: %s\n", err.c_str()); return 4; }
    std::fprintf(stderr, "listening on ws://127.0.0.1:%d (engine=%s provider=%s)\n",
                 cfg->port, engine->name().c_str(), engine->effectiveProvider().c_str());

    if (cfg->headless) {
        std::signal(SIGINT, onSignal);
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(cfg->durationSec);
        while (!g_stop && (cfg->durationSec <= 0 ||
                           std::chrono::steady_clock::now() < deadline)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

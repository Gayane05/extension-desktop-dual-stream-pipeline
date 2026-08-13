// desktop/src/stt/deepgram_engine.cpp
#include "stt/deepgram_engine.h"

#include <chrono>
#include <cstdio>

#include <ixwebsocket/IXWebSocket.h>
#include <rapidjson/document.h>

namespace dsp {

std::optional<TranscriptEvent> parseDeepgramMessage(StreamId s, const std::string& json,
                                                    double nowMs) {
    rapidjson::Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject()) return std::nullopt;
    if (!d.HasMember("type") || !d["type"].IsString() ||
        std::string(d["type"].GetString()) != "Results") return std::nullopt;
    if (!d.HasMember("channel") || !d["channel"].IsObject()) return std::nullopt;
    const auto& ch = d["channel"];
    if (!ch.HasMember("alternatives") || !ch["alternatives"].IsArray() ||
        ch["alternatives"].Empty()) return std::nullopt;
    const auto& alt = ch["alternatives"][0];
    if (!alt.HasMember("transcript") || !alt["transcript"].IsString()) return std::nullopt;
    std::string text = alt["transcript"].GetString();
    if (text.empty()) return std::nullopt;
    bool isFinal = d.HasMember("is_final") && d["is_final"].IsBool() && d["is_final"].GetBool();
    return TranscriptEvent{s, text, isFinal, nowMs};
}

static double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

DeepgramEngine::DeepgramEngine(EngineOptions opts, TranscriptCallback cb)
    : opts_(std::move(opts)), cb_(std::move(cb)) {}

DeepgramEngine::~DeepgramEngine() { stop(); }

bool DeepgramEngine::start(std::string& error) {
    if (opts_.deepgramKey.empty()) {
        error = "DEEPGRAM_API_KEY not set (required for --engine deepgram)";
        return false;
    }
    const std::string url =
        "wss://api.deepgram.com/v1/listen?encoding=linear16&sample_rate=16000"
        "&channels=1&interim_results=true&punctuate=true&model=nova-2";
    for (int i = 0; i < 2; ++i) {
        auto s = static_cast<StreamId>(i);
        ws_[i] = std::make_unique<ix::WebSocket>();
        ws_[i]->setUrl(url);
        ix::WebSocketHttpHeaders headers;
        headers["Authorization"] = "Token " + opts_.deepgramKey;
        ws_[i]->setExtraHeaders(headers);
        ws_[i]->enableAutomaticReconnection();  // per-stream reconnect w/ backoff
        ws_[i]->setOnMessageCallback([this, s](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message && !msg->binary) {
                if (auto ev = parseDeepgramMessage(s, msg->str, nowMs())) cb_(*ev);
            } else if (msg->type == ix::WebSocketMessageType::Error) {
                // Surface connection/TLS/HTTP failures instead of dying silently;
                // automatic reconnection keeps retrying in the background.
                std::fprintf(stderr, "deepgram[%s] connection error: %s (http %d)\n",
                             streamName(s), msg->errorInfo.reason.c_str(),
                             msg->errorInfo.http_status);
            } else if (msg->type == ix::WebSocketMessageType::Open) {
                std::fprintf(stderr, "deepgram[%s] connected\n", streamName(s));
            } else if (msg->type == ix::WebSocketMessageType::Close) {
                std::fprintf(stderr, "deepgram[%s] closed: %d %s\n", streamName(s),
                             msg->closeInfo.code, msg->closeInfo.reason.c_str());
            }
        });
        ws_[i]->start();
    }
    return true;
}

void DeepgramEngine::feed(StreamId s, const int16_t* samples, size_t n, double /*tsMs*/) {
    auto& ws = ws_[static_cast<int>(s)];
    if (!ws) return;
    ws->sendBinary(std::string(reinterpret_cast<const char*>(samples), n * 2));
}

void DeepgramEngine::stop() {
    for (auto& ws : ws_)
        if (ws) { ws->sendText(R"({"type":"CloseStream"})"); ws->stop(); ws.reset(); }
}

}  // namespace dsp

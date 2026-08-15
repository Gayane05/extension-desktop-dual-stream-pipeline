// desktop/src/stt/deepgram_engine.cpp
//
// See deepgram_engine.h for the class-level role/threading summary. Opens
// one Deepgram streaming WS connection per lane in start(), forwards raw
// PCM16 bytes in feed(), and turns Deepgram's JSON "Results" messages back
// into TranscriptEvents via the onMessage callback (which runs on
// ixwebsocket's own background thread per connection, not the feeder
// thread).
#include "stt/deepgram_engine.h"

#include <ixwebsocket/IXWebSocket.h>
#include <rapidjson/document.h>

#include <chrono>
#include <cstdio>

namespace dsp
{

std::optional<TranscriptEvent> parseDeepgramMessage(StreamId streamId, const std::string& json,
                                                    double nowMs)
{
    rapidjson::Document jsonDoc;
    jsonDoc.Parse(json.c_str());
    if (jsonDoc.HasParseError() || !jsonDoc.IsObject())
    {
        return std::nullopt;
    }
    if (!jsonDoc.HasMember("type") || !jsonDoc["type"].IsString() ||
        std::string(jsonDoc["type"].GetString()) != "Results")
    {
        return std::nullopt;
    }
    if (!jsonDoc.HasMember("channel") || !jsonDoc["channel"].IsObject())
    {
        return std::nullopt;
    }
    const auto& channel = jsonDoc["channel"];
    if (!channel.HasMember("alternatives") || !channel["alternatives"].IsArray() ||
        channel["alternatives"].Empty())
    {
        return std::nullopt;
    }
    const auto& alternative = channel["alternatives"][0];
    if (!alternative.HasMember("transcript") || !alternative["transcript"].IsString())
    {
        return std::nullopt;
    }
    std::string text = alternative["transcript"].GetString();
    if (text.empty())
    {
        return std::nullopt;
    }
    bool isFinal = jsonDoc.HasMember("is_final") && jsonDoc["is_final"].IsBool() &&
                   jsonDoc["is_final"].GetBool();
    return TranscriptEvent{streamId, text, isFinal, nowMs};
}

static double nowMs()
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

DeepgramEngine::DeepgramEngine(EngineOptions opts, TranscriptCallback cb)
    : opts_(std::move(opts)), cb_(std::move(cb))
{
}

DeepgramEngine::~DeepgramEngine()
{
    stop();
}

bool DeepgramEngine::start(std::string& error)
{
    if (opts_.deepgramKey.empty())
    {
        error =
            "no Deepgram API key configured -- set the DEEPGRAM_API_KEY environment "
            "variable or enter a key in the Settings screen";
        return false;
    }
    // encoding/sample_rate/channels here must match what feed() actually
    // sends (raw PCM16 mono @ 16 kHz, no header) -- Deepgram has no way to
    // infer the format itself for a raw linear16 stream, unlike the WAV/ogg
    // uploads its non-streaming API can sniff.
    const std::string url = "wss://api.deepgram.com/v1/listen?encoding=linear16&sample_rate=" +
                            std::to_string(kSampleRateHz) +
                            "&channels=1&interim_results=true&punctuate=true&model=nova-2";
    for (int i = 0; i < kStreamCount; ++i)
    {
        auto streamId = static_cast<StreamId>(i);
        ws_[i] = std::make_unique<ix::WebSocket>();
        ws_[i]->setUrl(url);
        ix::WebSocketHttpHeaders headers;
        headers["Authorization"] = "Token " + opts_.deepgramKey;
        ws_[i]->setExtraHeaders(headers);
        ws_[i]->enableAutomaticReconnection();  // Per-stream reconnect w/ backoff.
        ws_[i]->setOnMessageCallback([this, streamId](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message && !msg->binary)
            {
                if (auto transcriptEvent = parseDeepgramMessage(streamId, msg->str, nowMs()))
                {
                    cb_(*transcriptEvent);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                // Surface connection/TLS/HTTP failures instead of dying silently;
                // automatic reconnection keeps retrying in the background.
                std::fprintf(stderr, "deepgram[%s] connection error: %s (http %d)\n",
                             streamName(streamId), msg->errorInfo.reason.c_str(),
                             msg->errorInfo.http_status);
            }
            else if (msg->type == ix::WebSocketMessageType::Open)
            {
                std::fprintf(stderr, "deepgram[%s] connected\n", streamName(streamId));
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                std::fprintf(stderr, "deepgram[%s] closed: %d %s\n", streamName(streamId),
                             msg->closeInfo.code, msg->closeInfo.reason.c_str());
            }
        });
        ws_[i]->start();
    }
    return true;
}

void DeepgramEngine::feed(StreamId streamId, const int16_t* samples, size_t sampleCount,
                          double /*tsMs*/)
{
    auto& ws = ws_[static_cast<int>(streamId)];
    if (!ws)
    {
        return;
    }
    ws->sendBinary(std::string(reinterpret_cast<const char*>(samples), sampleCount * 2));
}

void DeepgramEngine::stop()
{
    for (auto& ws : ws_)
    {
        if (ws)
        {
            ws->sendText(R"({"type":"CloseStream"})");
            ws->stop();
            ws.reset();
        }
    }
}

}  // namespace dsp

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

// Milliseconds per second, for converting Deepgram's "start" offset.
constexpr double kMsPerSecond = 1000.0;

std::optional<TranscriptEvent> parseDeepgramMessage(StreamId streamId, const std::string& json,
                                                    double connectionEpochMs)
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
    // Stamp the event with the utterance's START so overlapping speech across
    // lanes sorts by who began talking first (see header comment).
    double tsMs = connectionEpochMs;
    if (jsonDoc.HasMember("start") && jsonDoc["start"].IsNumber())
    {
        tsMs += jsonDoc["start"].GetDouble() * kMsPerSecond;
    }
    return TranscriptEvent{streamId, text, isFinal, tsMs};
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

std::string buildDeepgramListenUrl(const std::string& language)
{
    // encoding/sample_rate/channels here must match what feed() actually
    // sends (raw PCM16 mono @ 16 kHz, no header) -- Deepgram has no way to
    // infer the format itself for a raw linear16 stream, unlike the WAV/ogg
    // uploads its non-streaming API can sniff.
    std::string url =
        "wss://api.deepgram.com/v1/listen?encoding=linear16&sample_rate=" +
        std::to_string(kSampleRateHz) +
        "&channels=1&interim_results=true&punctuate=true&model=nova-3&language=" + language;
    if (language == "multi")
    {
        // Deepgram's documented recommendation for multilingual
        // code-switching: a 100 ms endpoint so language flips inside one
        // utterance still split into clean results.
        url += "&endpointing=100";
    }
    return url;
}

std::string formatDeepgramConnectionError(const char* streamLabel, const std::string& reason,
                                          int httpStatus)
{
    std::string message = "deepgram[" + std::string(streamLabel) + "] connection failed";
    if (httpStatus > 0)
    {
        message += " (http " + std::to_string(httpStatus) + ")";
    }
    if (!reason.empty())
    {
        message += ": " + reason;
    }
    // 401/403 are the failures the user can actually fix from the app.
    if (httpStatus == 401 || httpStatus == 403)
    {
        message += " -- check the API key in Settings";
    }
    return message;
}

std::string DeepgramEngine::runtimeError() const
{
    std::lock_guard lock(connectionErrorMu_);
    for (const std::string& laneError : connectionError_)
    {
        if (!laneError.empty())
        {
            return laneError;
        }
    }
    return "";
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
    const std::string url = buildDeepgramListenUrl(opts_.language);
    for (int i = 0; i < kStreamCount; ++i)
    {
        auto streamId = static_cast<StreamId>(i);
        ws_[i] = std::make_unique<ix::WebSocket>();
        ws_[i]->setUrl(url);
        ix::WebSocketHttpHeaders headers;
        headers["Authorization"] = "Token " + opts_.deepgramKey;
        ws_[i]->setExtraHeaders(headers);
        ws_[i]->enableAutomaticReconnection();  // Per-stream reconnect w/ backoff.
        const int streamIndex = i;
        ws_[i]->setOnMessageCallback([this, streamId,
                                      streamIndex](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message && !msg->binary)
            {
                if (auto transcriptEvent =
                        parseDeepgramMessage(streamId, msg->str, connectionEpochMs_[streamIndex]))
                {
                    cb_(*transcriptEvent);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                // Surface connection/TLS/HTTP failures instead of dying silently;
                // automatic reconnection keeps retrying in the background. The
                // formatted message also feeds runtimeError() so the status
                // bar shows it -- start() succeeded before these async
                // connections resolved, so this is the only failure channel.
                std::fprintf(stderr, "deepgram[%s] connection error: %s (http %d)\n",
                             streamName(streamId), msg->errorInfo.reason.c_str(),
                             msg->errorInfo.http_status);
                std::lock_guard lock(connectionErrorMu_);
                connectionError_[streamIndex] =
                    formatDeepgramConnectionError(streamName(streamId), msg->errorInfo.reason,
                                                  static_cast<int>(msg->errorInfo.http_status));
            }
            else if (msg->type == ix::WebSocketMessageType::Open)
            {
                // The stream's "start" offsets count from (re)connection;
                // anchor them to wall-clock here.
                connectionEpochMs_[streamIndex] = nowMs();
                std::fprintf(stderr, "deepgram[%s] connected\n", streamName(streamId));
                std::lock_guard lock(connectionErrorMu_);
                connectionError_[streamIndex].clear();  // Healthy again.
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                std::fprintf(stderr, "deepgram[%s] closed: %d %s\n", streamName(streamId),
                             msg->closeInfo.code, msg->closeInfo.reason.c_str());
            }
        });
        ws_[i]->start();
    }
    // How often the idle KeepAlive is sent. Deepgram's no-data timeout is
    // ~10 s; 5 s keeps a healthy margin without meaningful traffic.
    constexpr int kKeepAliveIntervalMs = 5000;
    // Sleep in short slices so stop() never waits long for the join.
    constexpr int kKeepAliveSliceMs = 100;
    stopKeepAlive_ = false;
    keepAliveThread_ = std::thread([this, kKeepAliveIntervalMs, kKeepAliveSliceMs]() {
        int elapsedMs = 0;
        while (!stopKeepAlive_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kKeepAliveSliceMs));
            elapsedMs += kKeepAliveSliceMs;
            if (elapsedMs < kKeepAliveIntervalMs)
            {
                continue;
            }
            elapsedMs = 0;
            for (auto& ws : ws_)
            {
                if (ws)
                {
                    ws->sendText(R"({"type":"KeepAlive"})");
                }
            }
        }
    });
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
    // Stop the KeepAlive sender before tearing sockets down so it can never
    // touch a ws_[] slot mid-reset.
    stopKeepAlive_ = true;
    if (keepAliveThread_.joinable())
    {
        keepAliveThread_.join();
    }
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

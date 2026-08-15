// desktop/src/core/protocol.cpp
//
// Wire codec for the extension<->desktop link: turns raw WsServer bytes into
// AudioFrame/HelloInfo structs (parse side, called from ws_server.cpp's
// message callback) and turns outgoing status/error into JSON text (build
// side, called from pipeline.cpp). Malformed input from the wire is rejected
// here by returning std::nullopt/false rather than throwing, since the
// sender (a browser extension) is not a trusted peer.
#include "core/protocol.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cmath>
#include <cstring>

namespace dsp
{

// Layout: byte 0 = stream tag, bytes 1..8 = LE f64 capture timestamp (ms),
// remaining bytes = PCM16 mono samples. See kFrameHeaderSize in protocol.h.
std::optional<AudioFrame> parseBinaryFrame(const uint8_t* data, size_t len)
{
    if (len < kFrameHeaderSize)
    {
        return std::nullopt;
    }
    if (data[0] >= kStreamCount)
    {
        return std::nullopt;
    }
    size_t payload = len - kFrameHeaderSize;
    if (payload % 2 != 0)
    {
        return std::nullopt;
    }
    AudioFrame frame;
    frame.stream = static_cast<StreamId>(data[0]);
    // memcpy rather than a reinterpret_cast<double*> read: data+1 is not
    // guaranteed 8-byte aligned (it points one byte into a wire buffer), and
    // an unaligned double read plus the strict-aliasing rule both make a
    // direct cast undefined behavior. memcpy sidesteps both.
    std::memcpy(&frame.captureTsMs, data + 1, sizeof(double));  // LE host assumed (x86-64).
    // A NaN/Inf timestamp (malformed or malicious sender) would otherwise
    // flow straight into lastFrameMs_/streamState() and downstream JSON
    // (e.g. via TranscriptEvent), so reject it at the wire boundary.
    if (!std::isfinite(frame.captureTsMs))
    {
        return std::nullopt;
    }
    frame.samples.resize(payload / 2);
    if (payload > 0)
    {
        std::memcpy(frame.samples.data(), data + kFrameHeaderSize, payload);
    }
    return frame;
}

// Mirror of parseBinaryFrame's layout; used by desktop/tools/wav_client to
// synthesize frames identical in shape to what the extension sends.
std::vector<uint8_t> serializeBinaryFrame(StreamId streamId, double tsMs, const int16_t* samples,
                                          size_t sampleCount)
{
    std::vector<uint8_t> out(kFrameHeaderSize + sampleCount * 2);
    out[0] = static_cast<uint8_t>(streamId);
    // Same alignment/strict-aliasing reasoning as the parse side: memcpy into
    // the unaligned byte buffer instead of an aliasing double* write.
    std::memcpy(out.data() + 1, &tsMs, sizeof(double));
    if (sampleCount > 0)
    {
        std::memcpy(out.data() + kFrameHeaderSize, samples, sampleCount * 2);
    }
    return out;
}

static std::optional<rapidjson::Document> parseDoc(const std::string& text)
{
    rapidjson::Document jsonDoc;
    jsonDoc.Parse(text.c_str());
    if (jsonDoc.HasParseError() || !jsonDoc.IsObject())
    {
        return std::nullopt;
    }
    return jsonDoc;
}

std::optional<HelloInfo> parseHello(const std::string& jsonText)
{
    auto jsonDoc = parseDoc(jsonText);
    if (!jsonDoc)
    {
        return std::nullopt;
    }
    auto& doc = *jsonDoc;
    if (!doc.HasMember("type") || !doc["type"].IsString() ||
        std::string(doc["type"].GetString()) != "hello")
    {
        return std::nullopt;
    }
    HelloInfo helloInfo;
    if (doc.HasMember("version") && doc["version"].IsInt())
    {
        helloInfo.version = doc["version"].GetInt();
    }
    if (doc.HasMember("sampleRate") && doc["sampleRate"].IsInt())
    {
        helloInfo.sampleRate = doc["sampleRate"].GetInt();
    }
    if (doc.HasMember("channels") && doc["channels"].IsInt())
    {
        helloInfo.channels = doc["channels"].GetInt();
    }
    if (doc.HasMember("format") && doc["format"].IsString())
    {
        helloInfo.format = doc["format"].GetString();
    }
    return helloInfo;
}

bool isBye(const std::string& jsonText)
{
    auto jsonDoc = parseDoc(jsonText);
    return jsonDoc && jsonDoc->HasMember("type") && (*jsonDoc)["type"].IsString() &&
           std::string((*jsonDoc)["type"].GetString()) == "bye";
}

std::string buildStatusJson(const std::string& engine, const std::string& provider,
                            const std::string& micState, const std::string& tabState)
{
    rapidjson::StringBuffer jsonBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> jsonWriter(jsonBuffer);
    jsonWriter.StartObject();
    jsonWriter.Key("type");
    jsonWriter.String("status");
    jsonWriter.Key("engine");
    jsonWriter.String(engine.c_str());
    jsonWriter.Key("provider");
    jsonWriter.String(provider.c_str());
    jsonWriter.Key("streams");
    jsonWriter.StartObject();
    jsonWriter.Key("mic");
    jsonWriter.String(micState.c_str());
    jsonWriter.Key("tab");
    jsonWriter.String(tabState.c_str());
    jsonWriter.EndObject();
    jsonWriter.EndObject();
    return jsonBuffer.GetString();
}

std::string buildErrorJson(const std::string& message)
{
    rapidjson::StringBuffer jsonBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> jsonWriter(jsonBuffer);
    jsonWriter.StartObject();
    jsonWriter.Key("type");
    jsonWriter.String("error");
    jsonWriter.Key("message");
    jsonWriter.String(message.c_str());
    jsonWriter.EndObject();
    return jsonBuffer.GetString();
}

}  // namespace dsp

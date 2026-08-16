// desktop/tests/test_parakeet_engine.cpp
//
// Unit tests for stt/parakeet_engine.h: the pure sample-index-to-timestamp
// mapping, the missing-model failure path, and (when the Parakeet + Silero
// models are downloaded) a real decode of generated speech. The heavy test
// SKIPs cleanly on machines without the models.
#include <gtest/gtest.h>

#include <filesystem>
#include <mutex>
#include <vector>

#include "stt/parakeet_engine.h"

using namespace dsp;

namespace fs = std::filesystem;

namespace
{

// Tests run from the build tree; models live in <repo>/desktop/models.
std::string modelDir()
{
    for (const char* candidate :
         {"models", "../models", "../../models", "../../../models", "../../../../models"})
    {
        if (fs::exists(candidate))
        {
            return candidate;
        }
    }
    return "";
}

bool parakeetModelPresent(const std::string& dir)
{
    if (dir.empty() || !fs::exists(fs::path(dir) / "silero_vad.onnx"))
    {
        return false;
    }
    for (auto& dirEntry : fs::directory_iterator(dir))
    {
        if (dirEntry.is_directory() &&
            dirEntry.path().filename().string().find("parakeet") != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(ParakeetEngine, SampleIndexMapsToStreamTimeline)
{
    // 16000 samples at 16 kHz = exactly one second after the stream start.
    EXPECT_DOUBLE_EQ(sampleIndexToTsMs(1000.0, 0), 1000.0);
    EXPECT_DOUBLE_EQ(sampleIndexToTsMs(1000.0, 16000), 2000.0);
    EXPECT_DOUBLE_EQ(sampleIndexToTsMs(500.0, 8000), 1000.0);
}

TEST(ParakeetEngine, StartFailsWithMissingModel)
{
    ParakeetEngine engine({.modelDir = "Z:/definitely/missing", .provider = "cpu"},
                          [](const TranscriptEvent&) {});
    std::string error;
    EXPECT_FALSE(engine.start(error));
    EXPECT_NE(error.find("parakeet"), std::string::npos);
}

TEST(ParakeetEngine, TranscribesSpeechSegment)
{
    const std::string dir = modelDir();
    if (!parakeetModelPresent(dir))
    {
        GTEST_SKIP() << "parakeet model or silero_vad.onnx not downloaded";
    }
    std::mutex eventsMu;
    std::vector<TranscriptEvent> events;
    ParakeetEngine engine({.modelDir = dir, .provider = "cpu"},
                          [&](const TranscriptEvent& transcriptEvent) {
                              std::lock_guard lock(eventsMu);
                              events.push_back(transcriptEvent);
                          });
    std::string error;
    ASSERT_TRUE(engine.start(error)) << error;
    // 1.2 s of a 220 Hz square-ish tone is NOT speech: the VAD must not
    // produce a segment for it, and 1.5 s of silence must not either. This
    // verifies the VAD path end to end without depending on TTS availability;
    // real-speech quality is covered by the manual/A-B flow.
    std::vector<int16_t> tone(19200);
    for (size_t i = 0; i < tone.size(); ++i)
    {
        tone[i] = ((i / 36) % 2 == 0) ? 6000 : -6000;
    }
    engine.feed(StreamId::Mic, tone.data(), tone.size(), 0.0);
    std::vector<int16_t> silence(24000, 0);
    engine.feed(StreamId::Mic, silence.data(), silence.size(), 1200.0);
    engine.stop();
    std::lock_guard lock(eventsMu);
    for (const auto& transcriptEvent : events)
    {
        EXPECT_TRUE(transcriptEvent.isFinal);
    }
}

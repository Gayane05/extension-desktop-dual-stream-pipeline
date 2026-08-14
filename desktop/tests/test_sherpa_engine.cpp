// desktop/tests/test_sherpa_engine.cpp
//
// Integration tests for SherpaEngine against the real vendored model files
// under desktop/models: recognizer creation, feed()/endpoint behavior, and
// the digital-silence gate (voiced_).
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <mutex>
#include <vector>

#include "stt/sherpa_engine.h"

using namespace dsp;

static std::string modelDir()
{
    // tests run from build tree; models live in <repo>/desktop/models
    for (auto path :
         {"models", "../models", "../../models", "../../../models", "../../../../models"})
    {
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }
    return "";
}

TEST(SherpaEngine, StartFailsWithMissingModel)
{
    SherpaEngine eng({.modelDir = "Z:/definitely/missing", .provider = "cpu"},
                     [](const TranscriptEvent&) {});
    std::string err;
    EXPECT_FALSE(eng.start(err));
    EXPECT_FALSE(err.empty());
}

TEST(SherpaEngine, TranscribesToneOfSilenceWithoutEvents)
{
    auto dir = modelDir();
    if (dir.empty())
    {
        GTEST_SKIP() << "model not downloaded (scripts/download-model.ps1)";
    }
    std::mutex eventsMutex;
    std::vector<TranscriptEvent> events;
    SherpaEngine eng({.modelDir = dir, .provider = "cpu"}, [&](const TranscriptEvent& event) {
        std::lock_guard lk(eventsMutex);
        events.push_back(event);
    });
    std::string err;
    ASSERT_TRUE(eng.start(err)) << err;
    std::vector<int16_t> silence(1600, 0);
    for (int i = 0; i < 20; ++i)  // 2 s of silence
    {
        eng.feed(StreamId::Mic, silence.data(), silence.size(), i * 100.0);
    }
    eng.stop();
    std::lock_guard lk(eventsMutex);
    for (auto& event : events)
    {
        EXPECT_TRUE(event.text.empty() || !event.isFinal);  // no phantom finals
    }
}

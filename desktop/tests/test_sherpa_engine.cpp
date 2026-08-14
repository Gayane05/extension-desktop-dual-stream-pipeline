// desktop/tests/test_sherpa_engine.cpp
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
    for (auto p : {"models", "../models", "../../models", "../../../models", "../../../../models"})
    {
        if (std::filesystem::exists(p))
            return p;
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
        GTEST_SKIP() << "model not downloaded (scripts/download-model.ps1)";
    std::mutex mu;
    std::vector<TranscriptEvent> events;
    SherpaEngine eng({.modelDir = dir, .provider = "cpu"}, [&](const TranscriptEvent& e) {
        std::lock_guard lk(mu);
        events.push_back(e);
    });
    std::string err;
    ASSERT_TRUE(eng.start(err)) << err;
    std::vector<int16_t> silence(1600, 0);
    for (int i = 0; i < 20; ++i)  // 2 s of silence
        eng.feed(StreamId::Mic, silence.data(), silence.size(), i * 100.0);
    eng.stop();
    std::lock_guard lk(mu);
    for (auto& e : events)
        EXPECT_TRUE(e.text.empty() || !e.isFinal);  // no phantom finals
}

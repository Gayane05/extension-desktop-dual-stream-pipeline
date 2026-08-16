// desktop/src/stt/parakeet_engine.cpp
//
// See parakeet_engine.h for the class-level role/threading summary. This
// file covers model-file discovery (the Parakeet archive plus the separate
// silero_vad.onnx), recognizer/VAD construction, and the feed() flow:
// PCM16 -> float -> Silero VAD -> closed segments -> offline decode ->
// final TranscriptEvents stamped with the segment's start time. While a
// segment is still OPEN, the audio accumulated so far is re-decoded on a
// worker thread every ~kInterimIntervalSec and emitted as an interim event
// (see maybeQueueInterim / interimWorkerLoop), which is what makes this
// offline model feel live.
#include "stt/parakeet_engine.h"

#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <vector>

#include "stt/gpu_probe.h"

namespace fs = std::filesystem;

namespace dsp
{

namespace
{

// Silero VAD tuning. The speech-probability threshold and window size are
// the model's standard values; minimum speech length discards clicks.
constexpr float kVadThreshold = 0.5f;
constexpr float kVadMinSpeechSec = 0.25f;
constexpr int32_t kVadWindowSamples = 512;
// Force-close a segment that never pauses, mirroring the streaming engine's
// rule3 so run-on speech cannot delay output indefinitely.
constexpr float kVadMaxSpeechSec = 20.0f;
// How much audio the VAD may buffer while a segment is open.
constexpr float kVadBufferSec = 60.0f;
// The 0.6B model benefits from more threads than the small streaming ones.
constexpr int32_t kDecodeThreads = 4;

// Pseudo-streaming interim cadence (all measured in AUDIO time, not wall
// time, so behavior is deterministic under test). The first snapshot fires
// after this much speech, subsequent ones at the interval below; each
// re-decodes the whole open utterance so the interim line refines itself.
constexpr float kInterimFirstDelaySec = 1.0f;
constexpr float kInterimIntervalSec = 1.2f;
// Silero confirms speech only after its window + min-speech latency; reach
// this far back so the estimated utterance start covers the onset.
constexpr float kInterimLookbackSec = 0.6f;
// Rolling per-lane history the interim snapshots are cut from. Must exceed
// kVadMaxSpeechSec: an interim always re-decodes the whole open utterance
// (so its first words never vanish from the line), and max_speech is the
// longest an utterance can grow before the VAD force-closes it.
constexpr float kHistoryMaxSec = 25.0f;

constexpr int64_t secondsToSamples(float seconds)
{
    return static_cast<int64_t>(seconds * kSampleRateHz);
}

struct ParakeetFiles
{
    std::string encoder, decoder, joiner, tokens, sileroVad;
    bool complete() const
    {
        return !encoder.empty() && !decoder.empty() && !joiner.empty() && !tokens.empty() &&
               !sileroVad.empty();
    }
};

// Collects the transducer trio + tokens.txt from ONE directory. Unlike the
// streaming engine's discovery, int8 files are ACCEPTED (the published
// Parakeet archive is int8-only).
void scanParakeetDir(const fs::path& dir, ParakeetFiles& files)
{
    if (!fs::exists(dir))
    {
        return;
    }
    for (auto& dirEntry : fs::directory_iterator(dir))
    {
        auto name = dirEntry.path().filename().string();
        bool onnx = name.size() > 5 && name.substr(name.size() - 5) == ".onnx";
        if (name == "tokens.txt")
        {
            files.tokens = dirEntry.path().string();
        }
        else if (onnx && name.rfind("encoder", 0) == 0)
        {
            files.encoder = dirEntry.path().string();
        }
        else if (onnx && name.rfind("decoder", 0) == 0)
        {
            files.decoder = dirEntry.path().string();
        }
        else if (onnx && name.rfind("joiner", 0) == 0)
        {
            files.joiner = dirEntry.path().string();
        }
    }
}

// Looks for the Parakeet model in modelDir itself, then in one level of
// subdirectories whose name contains "parakeet" (the extracted archive
// layout). silero_vad.onnx is searched in modelDir and in the directory the
// model was found in.
ParakeetFiles findParakeetFiles(const fs::path& modelDir)
{
    ParakeetFiles files;
    fs::path foundIn = modelDir;
    scanParakeetDir(modelDir, files);
    if (files.encoder.empty() && fs::exists(modelDir))
    {
        for (auto& dirEntry : fs::directory_iterator(modelDir))
        {
            if (!dirEntry.is_directory())
            {
                continue;
            }
            const std::string dirName = dirEntry.path().filename().string();
            // Streaming Parakeet exports are a different model family that
            // runs through the streaming engine, not this offline one.
            if (dirName.find("parakeet") == std::string::npos ||
                dirName.find("streaming") != std::string::npos)
            {
                continue;
            }
            scanParakeetDir(dirEntry.path(), files);
            if (!files.encoder.empty())
            {
                foundIn = dirEntry.path();
                break;
            }
        }
    }
    for (const fs::path& candidate : {modelDir / "silero_vad.onnx", foundIn / "silero_vad.onnx"})
    {
        if (fs::exists(candidate))
        {
            files.sileroVad = candidate.string();
            break;
        }
    }
    return files;
}

}  // namespace

ParakeetEngine::ParakeetEngine(EngineOptions opts, TranscriptCallback cb)
    : opts_(std::move(opts)), cb_(std::move(cb))
{
}

ParakeetEngine::~ParakeetEngine()
{
    stop();
}

bool ParakeetEngine::createRecognizer(const std::string& provider, std::string& error)
{
    if (provider != "cpu" && !gpuProviderRuntimeAvailable(provider, error))
    {
        return false;
    }
    const ParakeetFiles files = findParakeetFiles(opts_.modelDir);
    if (!files.complete())
    {
        error = "parakeet model files not found in '" + opts_.modelDir +
                "' -- run scripts/download-model.ps1 -Model "
                "sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8 and -Model silero_vad.onnx";
        return false;
    }
    SherpaOnnxOfflineRecognizerConfig recognizerConfig{};
    recognizerConfig.model_config.transducer.encoder = files.encoder.c_str();
    recognizerConfig.model_config.transducer.decoder = files.decoder.c_str();
    recognizerConfig.model_config.transducer.joiner = files.joiner.c_str();
    recognizerConfig.model_config.tokens = files.tokens.c_str();
    recognizerConfig.model_config.num_threads = kDecodeThreads;
    recognizerConfig.model_config.provider = provider.c_str();
    // Parakeet TDT is a NeMo transducer export; sherpa-onnx needs the
    // explicit type to pick the right runtime implementation.
    recognizerConfig.model_config.model_type = "nemo_transducer";
    recognizerConfig.decoding_method = "greedy_search";
    recognizerConfig.feat_config.sample_rate = kSampleRateHz;
    recognizerConfig.feat_config.feature_dim = 80;
    // ONNX Runtime provider initialization can throw straight through the
    // sherpa C API (see SherpaEngine); convert to a clean failure so the cpu
    // fallback in start() can engage.
    try
    {
        recognizer_ = SherpaOnnxCreateOfflineRecognizer(&recognizerConfig);
    }
    catch (const std::exception& ex)
    {
        recognizer_ = nullptr;
        error = "recognizer creation failed (provider=" + provider + "): " + ex.what();
        return false;
    }
    catch (...)
    {
        recognizer_ = nullptr;
        error = "recognizer creation failed with an unknown error (provider=" + provider + ")";
        return false;
    }
    if (!recognizer_)
    {
        error = "failed to create parakeet recognizer (provider=" + provider + ")";
        return false;
    }
    effectiveProvider_ = provider;

    SherpaOnnxVadModelConfig vadConfig{};
    vadConfig.silero_vad.model = files.sileroVad.c_str();
    vadConfig.silero_vad.threshold = kVadThreshold;
    vadConfig.silero_vad.min_silence_duration = static_cast<float>(opts_.endpointSilenceSec);
    vadConfig.silero_vad.min_speech_duration = kVadMinSpeechSec;
    vadConfig.silero_vad.window_size = kVadWindowSamples;
    vadConfig.silero_vad.max_speech_duration = kVadMaxSpeechSec;
    vadConfig.sample_rate = kSampleRateHz;
    vadConfig.num_threads = 1;
    // The VAD is a ~2 MB model; it always runs on cpu regardless of the
    // recognizer's provider.
    vadConfig.provider = "cpu";
    for (int i = 0; i < kStreamCount; ++i)
    {
        vads_[i] = SherpaOnnxCreateVoiceActivityDetector(&vadConfig, kVadBufferSec);
        if (!vads_[i])
        {
            error = "failed to create the voice activity detector";
            return false;
        }
    }
    return true;
}

bool ParakeetEngine::start(std::string& error)
{
    if (!createRecognizer(opts_.provider, error))
    {
        if (opts_.provider != "cpu")
        {
            std::fprintf(stderr, "parakeet: provider '%s' unavailable (%s); falling back to cpu\n",
                         opts_.provider.c_str(), error.c_str());
            std::string cpuError;
            if (!createRecognizer("cpu", cpuError))
            {
                error += "; cpu fallback also failed: " + cpuError;
                return false;
            }
            error.clear();
        }
        else
        {
            return false;
        }
    }
    interimWorker_ = std::thread([this] { interimWorkerLoop(); });
    return true;
}

std::string ParakeetEngine::decodeAudioLocked(const float* samples, int32_t sampleCount)
{
    if (!recognizer_)
    {
        return "";
    }
    const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer_);
    if (!stream)
    {
        return "";
    }
    SherpaOnnxAcceptWaveformOffline(stream, kSampleRateHz, samples, sampleCount);
    SherpaOnnxDecodeOfflineStream(recognizer_, stream);
    const SherpaOnnxOfflineRecognizerResult* result = SherpaOnnxGetOfflineStreamResult(stream);
    std::string text = (result && result->text) ? result->text : "";
    SherpaOnnxDestroyOfflineRecognizerResult(result);
    SherpaOnnxDestroyOfflineStream(stream);
    return text;
}

void ParakeetEngine::decodeSegment(StreamId streamId, const float* samples, int32_t sampleCount,
                                   int32_t startSampleIndex)
{
    const int idx = static_cast<int>(streamId);
    std::lock_guard lock(decodeMu_);
    const std::string text = decodeAudioLocked(samples, sampleCount);
    // NOTE: cb_ runs while holding decodeMu_ -- it must not call back into
    // this engine (TranscriptModel::apply only takes its own unrelated lock).
    if (!text.empty())
    {
        cb_({streamId, text, true, sampleIndexToTsMs(streamStartTsMs_[idx], startSampleIndex)});
    }
}

void ParakeetEngine::drainSegments(StreamId streamId)
{
    const int idx = static_cast<int>(streamId);
    const SherpaOnnxVoiceActivityDetector* vad = vads_[idx];
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad))
    {
        const SherpaOnnxSpeechSegment* segment = SherpaOnnxVoiceActivityDetectorFront(vad);
        if (segment)
        {
            // Close the interim bookkeeping for this utterance BEFORE the
            // final decode: the generation bump makes any in-flight interim
            // for it stale, and the ordering with decodeMu_ guarantees a
            // stale interim can never be emitted after this final (see
            // interimWorkerLoop).
            utteranceGen_[idx].fetch_add(1);
            openStartAbs_[idx] = -1;
            lastFinalEndAbs_[idx] = static_cast<int64_t>(segment->start) + segment->n;
            {
                std::lock_guard lock(interimMu_);
                interimJobs_[idx].valid = false;  // Drop a superseded snapshot.
            }
            decodeSegment(streamId, segment->samples, segment->n, segment->start);
            SherpaOnnxDestroySpeechSegment(segment);
        }
        SherpaOnnxVoiceActivityDetectorPop(vad);
    }
}

void ParakeetEngine::maybeQueueInterim(StreamId streamId)
{
    const int idx = static_cast<int>(streamId);
    if (!SherpaOnnxVoiceActivityDetectorDetected(vads_[idx]))
    {
        // Not currently in speech. Any open utterance stays open (this may
        // be the pause the VAD has not yet confirmed as the segment end);
        // drainSegments resets the state once the segment actually closes.
        return;
    }
    if (openStartAbs_[idx] < 0)
    {
        // Speech just confirmed: estimate the utterance start a little
        // behind "now" to cover the VAD's confirmation latency, but never
        // inside audio that already belongs to a finalized segment.
        openStartAbs_[idx] = std::max(lastFinalEndAbs_[idx],
                                      absSampleCount_[idx] - secondsToSamples(kInterimLookbackSec));
        nextInterimAtAbs_[idx] = openStartAbs_[idx] + secondsToSamples(kInterimFirstDelaySec);
    }
    if (absSampleCount_[idx] < nextInterimAtAbs_[idx])
    {
        return;
    }
    nextInterimAtAbs_[idx] = absSampleCount_[idx] + secondsToSamples(kInterimIntervalSec);

    // Always decode from the utterance's START. A sliding window would be
    // cheaper, but its decode no longer contains the utterance's first words,
    // so the interim line's head would visibly vanish mid-speech. The VAD's
    // max_speech rule bounds the window (it force-closes run-on utterances),
    // and the closed part becomes an immutable final line.
    const int64_t begin = std::max(openStartAbs_[idx], historyBase_[idx]);
    const int64_t offset = begin - historyBase_[idx];
    if (offset >= static_cast<int64_t>(history_[idx].size()))
    {
        return;
    }
    InterimJob job;
    job.valid = true;
    job.samples.assign(history_[idx].begin() + offset, history_[idx].end());
    job.tsMs = sampleIndexToTsMs(streamStartTsMs_[idx], openStartAbs_[idx]);
    job.generation = utteranceGen_[idx].load();
    {
        std::lock_guard lock(interimMu_);
        interimJobs_[idx] = std::move(job);  // Latest snapshot wins.
    }
    interimCv_.notify_one();
}

void ParakeetEngine::interimWorkerLoop()
{
    for (;;)
    {
        InterimJob job;
        int idx = -1;
        {
            std::unique_lock lock(interimMu_);
            interimCv_.wait(lock, [this] {
                if (interimStop_)
                {
                    return true;
                }
                for (const InterimJob& pending : interimJobs_)
                {
                    if (pending.valid)
                    {
                        return true;
                    }
                }
                return false;
            });
            if (interimStop_)
            {
                // Pending interims are abandoned; stop() flushes finals.
                return;
            }
            for (int lane = 0; lane < kStreamCount; ++lane)
            {
                if (interimJobs_[lane].valid)
                {
                    idx = lane;
                    job = std::move(interimJobs_[lane]);
                    interimJobs_[lane].valid = false;
                    break;
                }
            }
        }
        if (idx < 0 || job.samples.empty())
        {
            continue;
        }
        // Decode, then re-check the generation and emit under the SAME hold
        // of decodeMu_. Final decodes bump the generation before taking
        // decodeMu_ (see drainSegments), so either this check sees the bump
        // and drops the interim, or the final decode is still waiting on the
        // mutex and its event is emitted after ours -- a stale interim can
        // never print after its final.
        std::lock_guard lock(decodeMu_);
        const std::string text =
            decodeAudioLocked(job.samples.data(), static_cast<int32_t>(job.samples.size()));
        if (utteranceGen_[idx].load() != job.generation)
        {
            continue;
        }
        if (text.empty() ||
            (lastInterimGen_[idx] == job.generation && text == lastInterimText_[idx]))
        {
            continue;
        }
        lastInterimText_[idx] = text;
        lastInterimGen_[idx] = job.generation;
        cb_({static_cast<StreamId>(idx), text, false, job.tsMs});
    }
}

void ParakeetEngine::feed(StreamId streamId, const int16_t* samples, size_t sampleCount,
                          double tsMs)
{
    // Divisor converting signed PCM16 samples to the [-1, 1) float range.
    constexpr float kPcmScale = 32768.0f;
    const int idx = static_cast<int>(streamId);
    if (!vads_[idx])
    {
        return;
    }
    if (streamStartTsMs_[idx] < 0.0)
    {
        streamStartTsMs_[idx] = tsMs;
    }
    std::vector<float> floatSamples(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i)
    {
        floatSamples[i] = samples[i] / kPcmScale;
    }
    // Keep a rolling history so interim snapshots can reach back to the
    // utterance's start; the VAD's own buffer is not readable mid-segment.
    history_[idx].insert(history_[idx].end(), floatSamples.begin(), floatSamples.end());
    absSampleCount_[idx] += static_cast<int64_t>(sampleCount);
    const int64_t historyMax = secondsToSamples(kHistoryMaxSec);
    if (static_cast<int64_t>(history_[idx].size()) > historyMax)
    {
        const int64_t drop = static_cast<int64_t>(history_[idx].size()) - historyMax;
        history_[idx].erase(history_[idx].begin(), history_[idx].begin() + drop);
        historyBase_[idx] += drop;
    }
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vads_[idx], floatSamples.data(),
                                                  static_cast<int32_t>(sampleCount));
    drainSegments(streamId);
    maybeQueueInterim(streamId);
}

void ParakeetEngine::stop()
{
    // Retire the interim worker before flushing finals so no interim can be
    // emitted during or after the flush.
    {
        std::lock_guard lock(interimMu_);
        interimStop_ = true;
    }
    interimCv_.notify_all();
    if (interimWorker_.joinable())
    {
        interimWorker_.join();
    }
    for (int i = 0; i < kStreamCount; ++i)
    {
        if (!vads_[i])
        {
            continue;
        }
        // Emit whatever speech was still buffered when capture ended.
        SherpaOnnxVoiceActivityDetectorFlush(vads_[i]);
        drainSegments(static_cast<StreamId>(i));
        SherpaOnnxDestroyVoiceActivityDetector(vads_[i]);
        vads_[i] = nullptr;
    }
    std::lock_guard lock(decodeMu_);
    if (recognizer_)
    {
        SherpaOnnxDestroyOfflineRecognizer(recognizer_);
        recognizer_ = nullptr;
    }
}

}  // namespace dsp

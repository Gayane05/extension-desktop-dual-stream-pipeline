// desktop/src/stt/sherpa_engine.cpp
//
// See sherpa_engine.h for the class-level role/threading summary. This file
// covers model-file discovery under Config::modelDir, recognizer
// construction (decoding method, endpointing rules), and the per-chunk
// feed()/decode/endpoint-check loop that turns PCM16 into TranscriptEvents.
#include "stt/sherpa_engine.h"

#include <sherpa-onnx/c-api/c-api.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace dsp
{

struct ModelFiles
{
    std::string encoder, decoder, joiner, tokens;
    bool complete() const
    {
        return !encoder.empty() && !decoder.empty() && !joiner.empty() && !tokens.empty();
    }
};

// Collects encoder/decoder/joiner (fp32, non-int8) + tokens.txt from ONE
// directory. All four must come from the same dir so that two extracted model
// archives side by side under models/ can never be mixed together.
static ModelFiles scanModelDir(const fs::path& dir)
{
    ModelFiles modelFiles;
    if (!fs::exists(dir))
    {
        return modelFiles;
    }
    for (auto& dirEntry : fs::directory_iterator(dir))
    {
        auto name = dirEntry.path().filename().string();
        bool onnx = name.size() > 5 && name.substr(name.size() - 5) == ".onnx";
        bool int8 = name.find("int8") != std::string::npos;
        if (name == "tokens.txt")
        {
            modelFiles.tokens = dirEntry.path().string();
        }
        else if (onnx && !int8 && name.rfind("encoder", 0) == 0)
        {
            modelFiles.encoder = dirEntry.path().string();
        }
        else if (onnx && !int8 && name.rfind("decoder", 0) == 0)
        {
            modelFiles.decoder = dirEntry.path().string();
        }
        else if (onnx && !int8 && name.rfind("joiner", 0) == 0)
        {
            modelFiles.joiner = dirEntry.path().string();
        }
    }
    return modelFiles;
}

// Looks in dir itself, then one level of nesting (models/<archive-name>/...);
// the first subdirectory containing a complete model wins.
static ModelFiles findModelFiles(const fs::path& dir)
{
    if (auto modelFiles = scanModelDir(dir); modelFiles.complete())
    {
        return modelFiles;
    }
    if (fs::exists(dir))
    {
        for (auto& dirEntry : fs::directory_iterator(dir))
        {
            if (dirEntry.is_directory())
            {
                if (auto modelFiles = scanModelDir(dirEntry.path()); modelFiles.complete())
                {
                    return modelFiles;
                }
            }
        }
    }
    return {};
}

SherpaEngine::SherpaEngine(EngineOptions opts, TranscriptCallback cb)
    : opts_(std::move(opts)), cb_(std::move(cb))
{
}

SherpaEngine::~SherpaEngine()
{
    stop();
}

// modified_beam_search's max_active_paths: number of decode hypotheses kept
// per step. 4 balances accuracy against decode cost.
constexpr int32_t kBeamActivePaths = 4;
// Zipformer feature extractor's Mel-filterbank dimension, fixed by the model.
constexpr int32_t kFeatureDim = 80;
// Endpoint rule1: finalize after this much trailing silence with no speech at
// all (a much longer fuse than rule2's --endpoint-silence, which fires after
// speech has already been heard).
constexpr float kRule1TrailingSilenceSec = 2.4f;
// Endpoint rule3: force-finalize an utterance that has run this long without
// ever pausing, so run-on speech still gets split into finals.
constexpr float kRule3MaxUtteranceSec = 20.0f;

// Verifies a GPU provider's runtime DLLs can actually load BEFORE handing
// the provider to ONNX Runtime: a missing dependency (e.g. cuDNN not on
// PATH) makes ORT abort the whole process -- not a catchable error -- so
// probing here is the only way the cpu fallback in start() can ever engage.
static bool gpuProviderRuntimeAvailable(const std::string& provider, std::string& error)
{
#ifdef _WIN32
    const char* requiredDlls[2] = {nullptr, nullptr};
    if (provider == "cuda")
    {
        requiredDlls[0] = "onnxruntime_providers_cuda.dll";
        requiredDlls[1] = "cudnn64_9.dll";
    }
    else if (provider == "tensorrt")
    {
        requiredDlls[0] = "onnxruntime_providers_tensorrt.dll";
    }
    for (const char* dllName : requiredDlls)
    {
        if (!dllName)
        {
            continue;
        }
        HMODULE probe = LoadLibraryA(dllName);
        if (!probe)
        {
            error = std::string(dllName) + " could not be loaded (Win32 error " +
                    std::to_string(GetLastError()) +
                    ") -- is the CUDA/cuDNN runtime installed and on PATH? (provider=" + provider +
                    ")";
            return false;
        }
        // Keep the module loaded: ORT will need it immediately anyway, and
        // holding the reference avoids a pointless unload/reload cycle.
    }
#endif
    return true;
}

bool SherpaEngine::createRecognizer(const std::string& provider, std::string& error)
{
    if (provider != "cpu" && !gpuProviderRuntimeAvailable(provider, error))
    {
        return false;
    }
    const ModelFiles files = findModelFiles(opts_.modelDir);
    if (!files.complete())
    {
        error =
            "model files not found in '" + opts_.modelDir + "' -- run scripts/download-model.ps1";
        return false;
    }
    SherpaOnnxOnlineRecognizerConfig recognizerConfig{};
    recognizerConfig.model_config.transducer.encoder = files.encoder.c_str();
    recognizerConfig.model_config.transducer.decoder = files.decoder.c_str();
    recognizerConfig.model_config.transducer.joiner = files.joiner.c_str();
    recognizerConfig.model_config.tokens = files.tokens.c_str();
    recognizerConfig.model_config.provider = provider.c_str();
    recognizerConfig.model_config.num_threads = 2;
    // modified_beam_search trades a little decode CPU for a meaningfully lower
    // word error rate vs greedy; decode cost is small next to the encoder.
    bool beam = opts_.decoding != "greedy";
    // NeMo streaming transducers (NVIDIA FastConformer exports) only support
    // greedy decoding -- sherpa-onnx hard-exits the process on any other
    // method. Their archives name the files plainly (encoder.onnx), unlike
    // the zipformers' encoder-epoch-...-chunk-... names, so downgrade
    // automatically instead of dying.
    if (beam && fs::path(files.encoder).filename() == "encoder.onnx")
    {
        std::fprintf(stderr,
                     "sherpa: NeMo-style model detected; beam decoding is unsupported for it, "
                     "using greedy\n");
        beam = false;
    }
    recognizerConfig.decoding_method = beam ? "modified_beam_search" : "greedy_search";
    if (beam)
    {
        recognizerConfig.max_active_paths = kBeamActivePaths;
    }
    recognizerConfig.feat_config.sample_rate = kSampleRateHz;
    recognizerConfig.feat_config.feature_dim = kFeatureDim;
    // Endpointing controls how utterances split into finals: rule2 fires
    // after a pause following speech (the sentence-splitting knob, exposed
    // as --endpoint-silence), rule1 after long silence with no speech, and
    // rule3 force-finalizes run-on speech that never pauses.
    recognizerConfig.enable_endpoint = 1;
    recognizerConfig.rule1_min_trailing_silence = kRule1TrailingSilenceSec;
    recognizerConfig.rule2_min_trailing_silence = static_cast<float>(opts_.endpointSilenceSec);
    recognizerConfig.rule3_min_utterance_length = kRule3MaxUtteranceSec;
    // ONNX Runtime provider initialization can throw C++ exceptions straight
    // through the sherpa C API (observed: the CUDA provider when cuDNN is not
    // on PATH). Left uncaught they reach std::terminate and kill the process
    // before start()'s cpu fallback can engage, so convert them into a clean
    // failure here.
    try
    {
        rec_ = SherpaOnnxCreateOnlineRecognizer(&recognizerConfig);
    }
    catch (const std::exception& ex)
    {
        rec_ = nullptr;
        error = "recognizer creation failed (provider=" + provider + "): " + ex.what();
        return false;
    }
    catch (...)
    {
        rec_ = nullptr;
        error = "recognizer creation failed with an unknown error (provider=" + provider + ")";
        return false;
    }
    if (!rec_)
    {
        error = "failed to create recognizer (provider=" + provider + ")";
        return false;
    }
    effectiveProvider_ = provider;
    return true;
}

bool SherpaEngine::start(std::string& error)
{
    // Takes mu_ even though the lifecycle contract (see header) guarantees no
    // concurrent feed()/stop() during start(): this makes the class
    // self-defending rather than contract-reliant, at zero cost (init-time
    // only). createRecognizer() is private and only ever called from here, so
    // it must not lock mu_ itself -- it would self-deadlock.
    std::lock_guard lock(mu_);
    if (!createRecognizer(opts_.provider, error))
    {
        if (opts_.provider != "cpu")
        {
            // Make the downgrade visible: the status bar only shows the
            // effective provider, not WHY the requested one failed.
            std::fprintf(stderr, "sherpa: provider '%s' unavailable (%s); falling back to cpu\n",
                         opts_.provider.c_str(), error.c_str());
            std::string cpuErr;
            if (createRecognizer("cpu", cpuErr))
            {
                error.clear();  // Succeeded on fallback; effectiveProvider_ says "cpu".
            }
            else
            {
                error += "; cpu fallback also failed: " + cpuErr;
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    streams_[0] = SherpaOnnxCreateOnlineStream(rec_);
    streams_[1] = SherpaOnnxCreateOnlineStream(rec_);
    return streams_[0] && streams_[1];
}

void SherpaEngine::feed(StreamId streamId, const int16_t* samples, size_t sampleCount, double tsMs)
{
    // Divisor converting signed PCM16 samples to the [-1, 1) float range
    // sherpa-onnx's feature extractor expects.
    constexpr float kPcmScale = 32768.0f;
    const int idx = static_cast<int>(streamId);
    int peak = 0;
    std::vector<float> floatSamples(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i)
    {
        int sampleValue = samples[i];
        if (sampleValue < 0)
        {
            sampleValue = -sampleValue;
        }
        if (sampleValue > peak)
        {
            peak = sampleValue;
        }
        floatSamples[i] = samples[i] / kPcmScale;
    }
    // ~0.3% of full scale: anything below is digital silence (muted source),
    // far under any real mic/tab noise floor. See voiced_ in the header.
    constexpr int kVoiceThreshold = 100;

    std::lock_guard lock(mu_);
    if (peak > kVoiceThreshold)
    {
        // First voiced chunk marks the utterance's start; emitted events
        // carry this timestamp (see utteranceStartTsMs_ in the header).
        if (!voiced_[idx])
        {
            utteranceStartTsMs_[idx] = tsMs;
        }
        voiced_[idx] = true;
    }
    if (!rec_ || !streams_[idx])
    {
        return;
    }
    auto* stream = streams_[idx];
    SherpaOnnxOnlineStreamAcceptWaveform(stream, kSampleRateHz, floatSamples.data(),
                                         static_cast<int32_t>(sampleCount));
    while (SherpaOnnxIsOnlineStreamReady(rec_, stream))
    {
        SherpaOnnxDecodeOnlineStream(rec_, stream);
    }

    const SherpaOnnxOnlineRecognizerResult* recognizerResult =
        SherpaOnnxGetOnlineStreamResult(rec_, stream);
    std::string text = (recognizerResult && recognizerResult->text) ? recognizerResult->text : "";
    SherpaOnnxDestroyOnlineRecognizerResult(recognizerResult);

    // Events are stamped with the utterance's START (first voiced chunk),
    // not the current chunk's time, so overlapping speech across lanes sorts
    // by who began talking first.
    const double utteranceTsMs = utteranceStartTsMs_[idx] >= 0.0 ? utteranceStartTsMs_[idx] : tsMs;

    // NOTE: cb_ runs while holding mu_ -- it must not call back into this
    // engine (TranscriptModel::apply only takes its own unrelated lock, so
    // this is safe today, but a future callback must not call feed()/stop()).
    if (SherpaOnnxOnlineStreamIsEndpoint(rec_, stream))
    {
        if (!text.empty() && voiced_[idx])
        {
            cb_({streamId, text, true, utteranceTsMs});
        }
        SherpaOnnxOnlineStreamReset(rec_, stream);
        lastInterim_[idx].clear();
        voiced_[idx] = false;  // Next utterance must re-prove it has signal.
        utteranceStartTsMs_[idx] = -1.0;
    }
    else if (text != lastInterim_[idx])
    {
        lastInterim_[idx] = text;
        if (!text.empty() && voiced_[idx])
        {
            cb_({streamId, text, false, utteranceTsMs});
        }
    }
}

void SherpaEngine::stop()
{
    std::lock_guard lock(mu_);
    for (auto*& stream : streams_)
    {
        if (stream)
        {
            SherpaOnnxDestroyOnlineStream(stream);
            stream = nullptr;
        }
    }
    if (rec_)
    {
        SherpaOnnxDestroyOnlineRecognizer(rec_);
        rec_ = nullptr;
    }
}

}  // namespace dsp

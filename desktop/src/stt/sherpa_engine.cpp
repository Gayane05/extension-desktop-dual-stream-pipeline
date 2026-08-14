// desktop/src/stt/sherpa_engine.cpp
//
// See sherpa_engine.h for the class-level role/threading summary. This file
// covers model-file discovery under Config::modelDir, recognizer
// construction (decoding method, endpointing rules), and the per-chunk
// feed()/decode/endpoint-check loop that turns PCM16 into TranscriptEvents.
#include "stt/sherpa_engine.h"

#include <sherpa-onnx/c-api/c-api.h>

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace dsp {

struct ModelFiles {
    std::string encoder, decoder, joiner, tokens;
    bool complete() const
    {
        return !encoder.empty() && !decoder.empty() && !joiner.empty() && !tokens.empty();
    }
};

// Collects encoder/decoder/joiner (fp32, non-int8) + tokens.txt from ONE
// directory. All four must come from the same dir so that two extracted model
// archives side by side under models/ can never be mixed together.
static ModelFiles scanModelDir(const fs::path& d)
{
    ModelFiles f;
    if (!fs::exists(d))
    {
        return f;
    }
    for (auto& e : fs::directory_iterator(d))
    {
        auto name = e.path().filename().string();
        bool onnx = name.size() > 5 && name.substr(name.size() - 5) == ".onnx";
        bool int8 = name.find("int8") != std::string::npos;
        if (name == "tokens.txt")
        {
            f.tokens = e.path().string();
        }
        else if (onnx && !int8 && name.rfind("encoder", 0) == 0)
        {
            f.encoder = e.path().string();
        }
        else if (onnx && !int8 && name.rfind("decoder", 0) == 0)
        {
            f.decoder = e.path().string();
        }
        else if (onnx && !int8 && name.rfind("joiner", 0) == 0)
        {
            f.joiner = e.path().string();
        }
    }
    return f;
}

// Looks in dir itself, then one level of nesting (models/<archive-name>/...);
// the first subdirectory containing a complete model wins.
static ModelFiles findModelFiles(const fs::path& dir)
{
    if (auto f = scanModelDir(dir); f.complete())
    {
        return f;
    }
    if (fs::exists(dir))
    {
        for (auto& e : fs::directory_iterator(dir))
        {
            if (e.is_directory())
            {
                if (auto f = scanModelDir(e.path()); f.complete())
                {
                    return f;
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

bool SherpaEngine::createRecognizer(const std::string& provider, std::string& error)
{
    const ModelFiles files = findModelFiles(opts_.modelDir);
    if (!files.complete())
    {
        error =
            "model files not found in '" + opts_.modelDir + "' -- run scripts/download-model.ps1";
        return false;
    }
    SherpaOnnxOnlineRecognizerConfig cfg{};
    cfg.model_config.transducer.encoder = files.encoder.c_str();
    cfg.model_config.transducer.decoder = files.decoder.c_str();
    cfg.model_config.transducer.joiner = files.joiner.c_str();
    cfg.model_config.tokens = files.tokens.c_str();
    cfg.model_config.provider = provider.c_str();
    cfg.model_config.num_threads = 2;
    // modified_beam_search trades a little decode CPU for a meaningfully lower
    // word error rate vs greedy; decode cost is small next to the encoder.
    const bool beam = opts_.decoding != "greedy";
    cfg.decoding_method = beam ? "modified_beam_search" : "greedy_search";
    if (beam)
    {
        cfg.max_active_paths = 4;
    }
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    // Endpointing controls how utterances split into finals: rule2 fires
    // after a pause following speech (the sentence-splitting knob, exposed
    // as --endpoint-silence), rule1 after long silence with no speech, and
    // rule3 force-finalizes run-on speech that never pauses.
    cfg.enable_endpoint = 1;
    cfg.rule1_min_trailing_silence = 2.4f;
    cfg.rule2_min_trailing_silence = static_cast<float>(opts_.endpointSilenceSec);
    cfg.rule3_min_utterance_length = 20.0f;
    rec_ = SherpaOnnxCreateOnlineRecognizer(&cfg);
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
    std::lock_guard lk(mu_);
    if (!createRecognizer(opts_.provider, error))
    {
        if (opts_.provider != "cpu")
        {
            std::string cpuErr;
            if (createRecognizer("cpu", cpuErr))
            {
                error.clear();  // succeeded on fallback; effectiveProvider_ says "cpu"
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

void SherpaEngine::feed(StreamId s, const int16_t* samples, size_t n, double tsMs)
{
    const int idx = static_cast<int>(s);
    int peak = 0;
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i)
    {
        int v = samples[i];
        if (v < 0)
        {
            v = -v;
        }
        if (v > peak)
        {
            peak = v;
        }
        f[i] = samples[i] / 32768.0f;
    }
    // ~0.3% of full scale: anything below is digital silence (muted source),
    // far under any real mic/tab noise floor. See voiced_ in the header.
    constexpr int kVoiceThreshold = 100;

    std::lock_guard lk(mu_);
    if (peak > kVoiceThreshold)
    {
        voiced_[idx] = true;
    }
    if (!rec_ || !streams_[idx])
    {
        return;
    }
    auto* stream = streams_[idx];
    SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, f.data(), static_cast<int32_t>(n));
    while (SherpaOnnxIsOnlineStreamReady(rec_, stream))
    {
        SherpaOnnxDecodeOnlineStream(rec_, stream);
    }

    const SherpaOnnxOnlineRecognizerResult* r = SherpaOnnxGetOnlineStreamResult(rec_, stream);
    std::string text = (r && r->text) ? r->text : "";
    SherpaOnnxDestroyOnlineRecognizerResult(r);

    // NOTE: cb_ runs while holding mu_ -- it must not call back into this
    // engine (TranscriptModel::apply only takes its own unrelated lock, so
    // this is safe today, but a future callback must not call feed()/stop()).
    if (SherpaOnnxOnlineStreamIsEndpoint(rec_, stream))
    {
        if (!text.empty() && voiced_[idx])
        {
            cb_({s, text, true, tsMs});
        }
        SherpaOnnxOnlineStreamReset(rec_, stream);
        lastInterim_[idx].clear();
        voiced_[idx] = false;  // next utterance must re-prove it has signal
    }
    else if (text != lastInterim_[idx])
    {
        lastInterim_[idx] = text;
        if (!text.empty() && voiced_[idx])
        {
            cb_({s, text, false, tsMs});
        }
    }
}

void SherpaEngine::stop()
{
    std::lock_guard lk(mu_);
    for (auto*& st : streams_)
    {
        if (st)
        {
            SherpaOnnxDestroyOnlineStream(st);
            st = nullptr;
        }
    }
    if (rec_)
    {
        SherpaOnnxDestroyOnlineRecognizer(rec_);
        rec_ = nullptr;
    }
}

}  // namespace dsp

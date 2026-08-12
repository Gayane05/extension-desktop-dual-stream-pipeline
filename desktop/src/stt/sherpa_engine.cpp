// desktop/src/stt/sherpa_engine.cpp
#include "stt/sherpa_engine.h"

#include <filesystem>
#include <vector>

#include <sherpa-onnx/c-api/c-api.h>

namespace fs = std::filesystem;

namespace dsp {

// Finds first file in dir matching prefix+".onnx"-ish patterns; descends one
// level if dir contains a single subdirectory (extracted archive layout).
static std::string findModelFile(const fs::path& dir, const std::string& prefix) {
    auto scan = [&](const fs::path& d) -> std::string {
        if (!fs::exists(d)) return "";
        for (auto& e : fs::directory_iterator(d)) {
            auto name = e.path().filename().string();
            bool ext = name.size() > 5 && name.substr(name.size() - 5) == ".onnx";
            if (prefix == "tokens" ? name == "tokens.txt"
                                   : (ext && name.rfind(prefix, 0) == 0 &&
                                      name.find("int8") == std::string::npos))
                return e.path().string();
        }
        return "";
    };
    if (auto f = scan(dir); !f.empty()) return f;
    // one level of nesting: models/<archive-name>/files
    if (fs::exists(dir))
        for (auto& e : fs::directory_iterator(dir))
            if (e.is_directory())
                if (auto f = scan(e.path()); !f.empty()) return f;
    return "";
}

SherpaEngine::SherpaEngine(EngineOptions opts, TranscriptCallback cb)
    : opts_(std::move(opts)), cb_(std::move(cb)) {}

SherpaEngine::~SherpaEngine() { stop(); }

bool SherpaEngine::createRecognizer(const std::string& provider, std::string& error) {
    const std::string encoder = findModelFile(opts_.modelDir, "encoder");
    const std::string decoder = findModelFile(opts_.modelDir, "decoder");
    const std::string joiner = findModelFile(opts_.modelDir, "joiner");
    const std::string tokens = findModelFile(opts_.modelDir, "tokens");
    if (encoder.empty() || decoder.empty() || joiner.empty() || tokens.empty()) {
        error = "model files not found in '" + opts_.modelDir +
                "' -- run scripts/download-model.ps1";
        return false;
    }
    SherpaOnnxOnlineRecognizerConfig cfg{};
    cfg.model_config.transducer.encoder = encoder.c_str();
    cfg.model_config.transducer.decoder = decoder.c_str();
    cfg.model_config.transducer.joiner = joiner.c_str();
    cfg.model_config.tokens = tokens.c_str();
    cfg.model_config.provider = provider.c_str();
    cfg.model_config.num_threads = 2;
    cfg.decoding_method = "greedy_search";
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    cfg.enable_endpoint = 1;
    cfg.rule1_min_trailing_silence = 2.4f;
    cfg.rule2_min_trailing_silence = 1.2f;
    cfg.rule3_min_utterance_length = 30.0f;
    rec_ = SherpaOnnxCreateOnlineRecognizer(&cfg);
    if (!rec_) {
        error = "failed to create recognizer (provider=" + provider + ")";
        return false;
    }
    effectiveProvider_ = provider;
    return true;
}

bool SherpaEngine::start(std::string& error) {
    // Takes mu_ even though the lifecycle contract (see header) guarantees no
    // concurrent feed()/stop() during start(): this makes the class
    // self-defending rather than contract-reliant, at zero cost (init-time
    // only). createRecognizer() is private and only ever called from here, so
    // it must not lock mu_ itself -- it would self-deadlock.
    std::lock_guard lk(mu_);
    if (!createRecognizer(opts_.provider, error)) {
        if (opts_.provider != "cpu") {
            std::string cpuErr;
            if (createRecognizer("cpu", cpuErr)) {
                error.clear();  // succeeded on fallback; effectiveProvider_ says "cpu"
            } else {
                error += "; cpu fallback also failed: " + cpuErr;
                return false;
            }
        } else {
            return false;
        }
    }
    streams_[0] = SherpaOnnxCreateOnlineStream(rec_);
    streams_[1] = SherpaOnnxCreateOnlineStream(rec_);
    return streams_[0] && streams_[1];
}

void SherpaEngine::feed(StreamId s, const int16_t* samples, size_t n, double tsMs) {
    const int idx = static_cast<int>(s);
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i) f[i] = samples[i] / 32768.0f;

    std::lock_guard lk(mu_);
    if (!rec_ || !streams_[idx]) return;
    auto* stream = streams_[idx];
    SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, f.data(), static_cast<int32_t>(n));
    while (SherpaOnnxIsOnlineStreamReady(rec_, stream))
        SherpaOnnxDecodeOnlineStream(rec_, stream);

    const SherpaOnnxOnlineRecognizerResult* r = SherpaOnnxGetOnlineStreamResult(rec_, stream);
    std::string text = (r && r->text) ? r->text : "";
    SherpaOnnxDestroyOnlineRecognizerResult(r);

    // NOTE: cb_ runs while holding mu_ -- it must not call back into this
    // engine (TranscriptModel::apply only takes its own unrelated lock, so
    // this is safe today, but a future callback must not call feed()/stop()).
    if (SherpaOnnxOnlineStreamIsEndpoint(rec_, stream)) {
        if (!text.empty()) cb_({s, text, true, tsMs});
        SherpaOnnxOnlineStreamReset(rec_, stream);
        lastInterim_[idx].clear();
    } else if (text != lastInterim_[idx]) {
        lastInterim_[idx] = text;
        if (!text.empty()) cb_({s, text, false, tsMs});
    }
}

void SherpaEngine::stop() {
    std::lock_guard lk(mu_);
    for (auto*& st : streams_)
        if (st) {
            SherpaOnnxDestroyOnlineStream(st);
            st = nullptr;
        }
    if (rec_) {
        SherpaOnnxDestroyOnlineRecognizer(rec_);
        rec_ = nullptr;
    }
}

}  // namespace dsp

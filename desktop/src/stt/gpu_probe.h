// desktop/src/stt/gpu_probe.h
//
// Pre-flight check shared by the local engines (SherpaEngine,
// ParakeetEngine) before they hand a GPU provider to ONNX Runtime. A missing
// runtime dependency (e.g. cuDNN not on PATH, or a display driver in a bad
// state) makes ORT abort the whole process -- not a catchable error -- so
// probe-loading the provider DLLs first is the only way a cpu fallback can
// ever engage.
#pragma once
#include <string>

namespace dsp
{

// Returns true when `provider` ("cuda"/"tensorrt") has its runtime DLLs
// loadable in this process; on failure returns false with a human-readable
// reason in `error`. Always true for "cpu" and on non-Windows builds.
bool gpuProviderRuntimeAvailable(const std::string& provider, std::string& error);

}  // namespace dsp

// desktop/src/stt/gpu_probe.cpp
//
// See gpu_probe.h. The probe keeps successfully loaded modules resident:
// ONNX Runtime needs them immediately afterwards anyway, and holding the
// reference avoids a pointless unload/reload cycle.
#include "stt/gpu_probe.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace dsp
{

bool gpuProviderRuntimeAvailable(const std::string& provider, std::string& error)
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
    }
#else
    (void)provider;
    (void)error;
#endif
    return true;
}

}  // namespace dsp

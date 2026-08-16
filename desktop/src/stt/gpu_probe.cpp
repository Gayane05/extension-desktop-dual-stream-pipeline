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
    // Probe the CUDA runtime dependencies, NOT ONNX Runtime's own provider
    // DLL: loading onnxruntime_providers_cuda.dll outside ORT's controlled
    // startup fails its initialization (error 1114) even on machines where
    // CUDA works, while ORT's own load of it succeeds. The process-killing
    // abort this probe exists to prevent is triggered specifically by these
    // third-party runtime DLLs being absent from the search path.
    const char* requiredDlls[3] = {nullptr, nullptr, nullptr};
    if (provider == "cuda")
    {
        requiredDlls[0] = "cudart64_12.dll";
        requiredDlls[1] = "cublas64_12.dll";
        requiredDlls[2] = "cudnn64_9.dll";
    }
    else if (provider == "tensorrt")
    {
        // TensorRT is documented as experimental; its core runtime DLL is
        // the meaningful availability signal.
        requiredDlls[0] = "cudart64_12.dll";
        requiredDlls[1] = "nvinfer_10.dll";
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

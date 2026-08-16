# desktop/CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(dual_stream_transcriber CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A machine-global vcpkg MSBuild integration (if present) injects its own
# include/lib paths into every generated VS project, silently overriding
# Conan's OpenSSL/zlib at link time. Observed symptom: transcriber.exe
# importing vcpkg's libssl-3-x64.dll and failing every TLS handshake
# ("peer didn't present a X509 certificate"). Conan owns ALL third-party
# dependencies in this project, so shut vcpkg out of the build entirely.
set(CMAKE_VS_GLOBALS "VcpkgEnabled=false")

find_package(ixwebsocket REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(imgui REQUIRED)
find_package(GTest REQUIRED)
find_package(sherpa-onnx REQUIRED)

# Locate the sherpa-onnx package's runtime DLLs (sherpa-onnx-c-api.dll and the
# onnxruntime DLLs it loads). They must sit next to every executable that links
# sherpa-onnx, or the process fails to start.
#
# $<TARGET_RUNTIME_DLLS:...> cannot help here: Conan's CMakeDeps declares its
# packages as INTERFACE IMPORTED targets, which carry no IMPORTED_LOCATION, so
# the generator expression expands to nothing. Resolve the package's bin/
# directory from the CMakeDeps data variables instead.
#
# Conan writes one data file per installed build_type, so a machine with both a
# Debug and a Release package gets both variables. The Visual Studio generator
# is multi-config: a single configure serves every `--config`, so the staged
# directory must be chosen by $<CONFIG> at build time rather than picked once
# here -- otherwise a Debug build would be handed Release DLLs (mismatched CRT,
# and a debugger pointed at the wrong binaries).
if(WIN32)
    set(_sherpa_debug_bin "")
    if(sherpa-onnx_PACKAGE_FOLDER_DEBUG
       AND EXISTS "${sherpa-onnx_PACKAGE_FOLDER_DEBUG}/bin/sherpa-onnx-c-api.dll")
        set(_sherpa_debug_bin "${sherpa-onnx_PACKAGE_FOLDER_DEBUG}/bin")
    endif()

    # Release, RelWithDebInfo and MinSizeRel all link the release CRT and are
    # served by whichever of those packages is installed.
    set(_sherpa_release_bin "")
    foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
        if(NOT _sherpa_release_bin
           AND sherpa-onnx_PACKAGE_FOLDER_${_cfg}
           AND EXISTS "${sherpa-onnx_PACKAGE_FOLDER_${_cfg}}/bin/sherpa-onnx-c-api.dll")
            set(_sherpa_release_bin "${sherpa-onnx_PACKAGE_FOLDER_${_cfg}}/bin")
        endif()
    endforeach()

    if(_sherpa_debug_bin AND _sherpa_release_bin)
        set(SHERPA_ONNX_BIN_DIR "$<IF:$<CONFIG:Debug>,${_sherpa_debug_bin},${_sherpa_release_bin}>")
    elseif(_sherpa_release_bin)
        # Release-only install (the normal case): also used for Debug builds.
        set(SHERPA_ONNX_BIN_DIR "${_sherpa_release_bin}")
    elseif(_sherpa_debug_bin)
        set(SHERPA_ONNX_BIN_DIR "${_sherpa_debug_bin}")
    else()
        message(FATAL_ERROR
            "Could not find sherpa-onnx-c-api.dll in the sherpa-onnx Conan package. "
            "Rebuild it with: cd recipes/sherpa-onnx && conan create . --version 1.13.5 "
            "--profile:all=../../conan_profiles/default --build=missing -s build_type=Release")
    endif()
endif()

# Stage the sherpa-onnx runtime next to `target`'s executable so it can be run
# straight from the build tree (and so ctest works without a PATH dance).
#
# The whole bin/ directory is copied rather than a file(GLOB) list: the glob
# would be evaluated at configure time (going stale if the package changes) and
# cannot be expressed over a $<CONFIG>-dependent path anyway.
function(dsp_copy_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                    "${SHERPA_ONNX_BIN_DIR}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
            COMMENT "Staging sherpa-onnx runtime DLLs next to ${target}")
    endif()
endfunction()

add_library(dsp_core STATIC)
# core sources appended by later tasks:
target_sources(dsp_core PRIVATE
    src/core/version.h
    src/core/version.cpp
    src/core/protocol.h
    src/core/protocol.cpp
    src/core/secret_store.h
    src/core/secret_store.cpp
    src/stt/gpu_probe.h
    src/stt/gpu_probe.cpp
    src/stt/parakeet_engine.h
    src/stt/parakeet_engine.cpp
    src/core/wav.h
    src/core/wav.cpp
    src/app/pipeline.h
    src/app/pipeline.cpp
    src/app/transcript_model.h
    src/app/transcript_model.cpp
    src/app/config.h
    src/app/config.cpp
    src/net/ws_server.h
    src/net/ws_server.cpp
    src/stt/stt_engine.h
    src/stt/sherpa_engine.h
    src/stt/sherpa_engine.cpp
    src/stt/deepgram_engine.h
    src/stt/deepgram_engine.cpp
    src/ui/save_transcript.h
    src/ui/save_transcript.cpp
)
set_target_properties(dsp_core PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(dsp_core PUBLIC src)
# crypt32 provides DPAPI (CryptProtectData) and base64 for core/secret_store.
target_link_libraries(dsp_core PUBLIC rapidjson ixwebsocket::ixwebsocket sherpa-onnx::sherpa-onnx
                                      crypt32)

enable_testing()
add_executable(dsp_tests
    tests/test_smoke.cpp
    tests/test_protocol.cpp
    tests/test_spsc_ring.cpp
    tests/test_transcript_model.cpp
    tests/test_config.cpp
    tests/test_ws_server.cpp
    tests/test_sherpa_link.cpp
    tests/test_sherpa_engine.cpp
    tests/test_deepgram_parse.cpp
    tests/test_wav.cpp
    tests/test_save_transcript.cpp
    tests/test_secret_store.cpp
    tests/test_parakeet_engine.cpp
)
target_link_libraries(dsp_tests PRIVATE dsp_core gtest::gtest)
dsp_copy_runtime_dlls(dsp_tests)
include(GoogleTest)
gtest_discover_tests(dsp_tests DISCOVERY_MODE PRE_TEST)

# imgui ships backend sources in the Conan package's res/bindings.
#
# The brief's original trick (get_target_property(... INTERFACE_INCLUDE_DIRECTORIES)
# then cmake_path(GET ... PARENT_PATH)) does not land on res/bindings here: on
# this multi-config (Visual Studio) generator, Conan's CMakeDeps wraps that
# property in a $<$<CONFIG:Release>:...> generator expression, which
# cmake_path() cannot parse at configure time -- it treats the whole
# genex-wrapped string as one opaque path segment. Resolve the package folder
# from the CMakeDeps-generated <pkg>_PACKAGE_FOLDER_<CONFIG> variable instead
# (the same pattern already used above for sherpa-onnx's DLL directory), which
# is a plain configure-time string.
set(_IMGUI_PKG "")
foreach(_cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
    if(NOT _IMGUI_PKG AND imgui_PACKAGE_FOLDER_${_cfg})
        set(_IMGUI_PKG "${imgui_PACKAGE_FOLDER_${_cfg}}")
    endif()
endforeach()
if(NOT _IMGUI_PKG)
    message(FATAL_ERROR "Could not resolve the imgui Conan package folder")
endif()
set(IMGUI_BINDINGS_DIR "${_IMGUI_PKG}/res/bindings")
if(NOT EXISTS "${IMGUI_BINDINGS_DIR}/imgui_impl_dx11.cpp")
    message(FATAL_ERROR "imgui bindings not found at ${IMGUI_BINDINGS_DIR}")
endif()
add_library(imgui_backends STATIC
    "${IMGUI_BINDINGS_DIR}/imgui_impl_win32.cpp"
    "${IMGUI_BINDINGS_DIR}/imgui_impl_dx11.cpp")
target_include_directories(imgui_backends PUBLIC "${IMGUI_BINDINGS_DIR}")
target_link_libraries(imgui_backends PUBLIC imgui::imgui d3d11 dxgi)

add_executable(transcriber src/main.cpp src/ui/main_window.cpp)
target_link_libraries(transcriber PRIVATE dsp_core imgui_backends)
dsp_copy_runtime_dlls(transcriber)

add_executable(wav_client tools/wav_client/main.cpp)
target_link_libraries(wav_client PRIVATE dsp_core)
dsp_copy_runtime_dlls(wav_client)

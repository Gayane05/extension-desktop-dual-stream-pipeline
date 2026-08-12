# desktop/CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(dual_stream_transcriber CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
# directory from the CMakeDeps data variables instead. Deliberately not a cache
# entry -- the path changes whenever the package is rebuilt.
if(WIN32)
    set(SHERPA_ONNX_BIN_DIR "")
    foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
        set(_candidate "${sherpa-onnx_PACKAGE_FOLDER_${_cfg}}/bin")
        if(sherpa-onnx_PACKAGE_FOLDER_${_cfg} AND EXISTS "${_candidate}/sherpa-onnx-c-api.dll")
            set(SHERPA_ONNX_BIN_DIR "${_candidate}")
            break()
        endif()
    endforeach()
    if(NOT SHERPA_ONNX_BIN_DIR)
        message(FATAL_ERROR
            "Could not find sherpa-onnx-c-api.dll in the sherpa-onnx Conan package. "
            "Rebuild it with: cd recipes/sherpa-onnx && conan create . --version 1.13.5 "
            "--profile:all=../../conan_profiles/default --build=missing -s build_type=Release")
    endif()
    file(GLOB SHERPA_ONNX_RUNTIME_DLLS "${SHERPA_ONNX_BIN_DIR}/*.dll")
endif()

# Stage the sherpa-onnx runtime next to `target`'s executable so it can be run
# straight from the build tree (and so ctest works without a PATH dance).
function(dsp_copy_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${SHERPA_ONNX_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
            COMMAND_EXPAND_LISTS
            VERBATIM
            COMMENT "Copying sherpa-onnx runtime DLLs next to ${target}")
    endif()
endfunction()

add_library(dsp_core STATIC)
# core sources appended by later tasks:
target_sources(dsp_core PRIVATE
    src/core/version.h
    src/core/version.cpp
    src/core/protocol.h
    src/core/protocol.cpp
    src/app/transcript_model.h
    src/app/transcript_model.cpp
    src/app/config.h
    src/app/config.cpp
    src/net/ws_server.h
    src/net/ws_server.cpp
)
set_target_properties(dsp_core PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(dsp_core PUBLIC src)
target_link_libraries(dsp_core PUBLIC rapidjson ixwebsocket::ixwebsocket sherpa-onnx::sherpa-onnx)

enable_testing()
add_executable(dsp_tests
    tests/test_smoke.cpp
    tests/test_protocol.cpp
    tests/test_spsc_ring.cpp
    tests/test_transcript_model.cpp
    tests/test_config.cpp
    tests/test_ws_server.cpp
    tests/test_sherpa_link.cpp
)
target_link_libraries(dsp_tests PRIVATE dsp_core gtest::gtest)
dsp_copy_runtime_dlls(dsp_tests)
include(GoogleTest)
gtest_discover_tests(dsp_tests DISCOVERY_MODE PRE_TEST)

# desktop/CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(dual_stream_transcriber CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ixwebsocket REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(imgui REQUIRED)
find_package(GTest REQUIRED)

add_library(dsp_core STATIC)
# core sources appended by later tasks:
target_sources(dsp_core PRIVATE
    src/core/version.h
    src/core/version.cpp
    src/core/protocol.h
    src/core/protocol.cpp
)
set_target_properties(dsp_core PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(dsp_core PUBLIC src)
target_link_libraries(dsp_core PUBLIC rapidjson ixwebsocket::ixwebsocket)

enable_testing()
add_executable(dsp_tests
    tests/test_smoke.cpp
    tests/test_protocol.cpp
    tests/test_spsc_ring.cpp
)
target_link_libraries(dsp_tests PRIVATE dsp_core gtest::gtest)
include(GoogleTest)
gtest_discover_tests(dsp_tests DISCOVERY_MODE PRE_TEST)

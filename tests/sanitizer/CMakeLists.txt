cmake_minimum_required(VERSION 3.20)
project(webm_player_sanitizer_tests LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Project root for includes
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(CPP_ROOT "${PROJECT_ROOT}/cpp")

# Force macOS to be recognized as iOS by Platform.h.
add_compile_definitions(
    TARGET_OS_IOS=1
    TARGET_OS_IPHONE=1
    FLOATING_POINT=1
)

# Warnings
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-macro-redefined)

# Sanitizer selection via -DSANITIZER=address|thread|undefined
if(NOT DEFINED SANITIZER)
    set(SANITIZER "address" CACHE STRING "Sanitizer to use: address, thread, undefined")
endif()

if(SANITIZER STREQUAL "address")
    message(STATUS "Building with AddressSanitizer + LeakSanitizer")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address)
elseif(SANITIZER STREQUAL "thread")
    message(STATUS "Building with ThreadSanitizer")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
elseif(SANITIZER STREQUAL "undefined")
    message(STATUS "Building with UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=undefined)
elseif(SANITIZER STREQUAL "none")
    message(STATUS "Building without sanitizer (RT safety interposition)")
    add_compile_options(-fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -O0)
else()
    message(FATAL_ERROR "Unknown SANITIZER=${SANITIZER}. Use 'address', 'thread', 'undefined', or 'none'.")
endif()

# Opus library - prefer Homebrew for macOS-native build
find_path(OPUS_INCLUDE_DIR opus/opus.h
    HINTS /opt/homebrew/include /usr/local/include
)
find_library(OPUS_LIBRARY opus
    HINTS /opt/homebrew/lib /usr/local/lib
)
if(NOT OPUS_INCLUDE_DIR OR NOT OPUS_LIBRARY)
    message(FATAL_ERROR "Opus not found. Install via: brew install opus")
endif()
message(STATUS "Opus include: ${OPUS_INCLUDE_DIR}")
message(STATUS "Opus library: ${OPUS_LIBRARY}")

# SpeexDSP resampler — vendored sources (no system dependency)
set(SPEEXDSP_ROOT "${CPP_ROOT}/third_party/speexdsp")
add_library(speexdsp_resampler STATIC "${SPEEXDSP_ROOT}/libspeexdsp/resample.c")
target_include_directories(speexdsp_resampler PUBLIC
    "${SPEEXDSP_ROOT}/include"
    "${SPEEXDSP_ROOT}/libspeexdsp"
)
target_compile_definitions(speexdsp_resampler PRIVATE FLOATING_POINT=1 EXPORT=)
set(SPEEXDSP_LIBRARY speexdsp_resampler)

# Common include paths (playback-only, no capture)
include_directories(
    ${CPP_ROOT}
    ${CPP_ROOT}/common
    ${CPP_ROOT}/playback
    ${CPP_ROOT}/video
    ${CPP_ROOT}/demux
    ${OPUS_INCLUDE_DIR}/opus
    ${SPEEXDSP_ROOT}/include
    ${SPEEXDSP_ROOT}/libspeexdsp
    ${PROJECT_ROOT}/android/src/main/jni
    ${PROJECT_ROOT}/ios
)

# libwebm mkvparser source (vendored)
set(LIBWEBM_ROOT "${CPP_ROOT}/third_party/libwebm")

# ---- LSAN test (leak + memory tests) ----
add_executable(test_lsan
    test_lsan.cpp
    ${CPP_ROOT}/common/ClipIndex.cpp
)
target_link_libraries(test_lsan PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- TSAN test (threading tests) ----
add_executable(test_tsan
    test_tsan.cpp
    ${CPP_ROOT}/common/ClipIndex.cpp
)
target_compile_definitions(test_tsan PRIVATE UNIT_TEST=1)
target_link_libraries(test_tsan PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- UBSAN test (undefined behavior) ----
add_executable(test_ubsan
    test_ubsan.cpp
    ${CPP_ROOT}/common/ClipIndex.cpp
    ${CPP_ROOT}/demux/WebmDemuxer.cpp
    ${CPP_ROOT}/demux/WebmTrackParser.cpp
    ${CPP_ROOT}/demux/WebmBlockParser.cpp
    ${LIBWEBM_ROOT}/mkvparser/mkvparser.cc
)
target_include_directories(test_ubsan PRIVATE ${LIBWEBM_ROOT})
target_link_libraries(test_ubsan PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- Route handler test (pure C++ reactive routing, no Opus/DSP dependencies) ----
add_executable(test_route_handler
    test_route_handler.cpp
)

# ---- Demuxer fuzz test ----

option(FUZZ "Build with libFuzzer instrumentation (requires clang)" OFF)

if(FUZZ)
    # libFuzzer mode: clang provides main() and coverage-guided mutation
    add_executable(fuzz_demuxer
        fuzz_demuxer.cpp
        ${CPP_ROOT}/demux/WebmDemuxer.cpp
    ${CPP_ROOT}/demux/WebmTrackParser.cpp
    ${CPP_ROOT}/demux/WebmBlockParser.cpp
        ${LIBWEBM_ROOT}/mkvparser/mkvparser.cc
    )
    target_include_directories(fuzz_demuxer PRIVATE ${LIBWEBM_ROOT})
    target_compile_options(fuzz_demuxer PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_demuxer PRIVATE -fsanitize=fuzzer)
    target_compile_definitions(fuzz_demuxer PRIVATE FUZZING_BUILD_MODE_INSTRUMENTATION)
else()
    # Standalone mode: built-in mutation harness with ASAN, runs in CI
    add_executable(fuzz_demuxer
        fuzz_demuxer.cpp
        ${CPP_ROOT}/demux/WebmDemuxer.cpp
    ${CPP_ROOT}/demux/WebmTrackParser.cpp
    ${CPP_ROOT}/demux/WebmBlockParser.cpp
        ${LIBWEBM_ROOT}/mkvparser/mkvparser.cc
    )
    target_include_directories(fuzz_demuxer PRIVATE ${LIBWEBM_ROOT})
endif()

# ---- End-to-end demux test (real WebM stream, validates full pipeline) ----
add_executable(test_e2e_demux
    test_e2e_demux.cpp
    ${CPP_ROOT}/demux/WebmDemuxer.cpp
    ${CPP_ROOT}/demux/WebmTrackParser.cpp
    ${CPP_ROOT}/demux/WebmBlockParser.cpp
    ${LIBWEBM_ROOT}/mkvparser/mkvparser.cc
)
target_include_directories(test_e2e_demux PRIVATE ${LIBWEBM_ROOT})
target_link_libraries(test_e2e_demux PRIVATE ${OPUS_LIBRARY})

# ---- Temporal stress tests (behavioral verification) ----
add_executable(test_temporal_stress
    test_temporal_stress.cpp
)
target_compile_definitions(test_temporal_stress PRIVATE PROJECT_ROOT="${PROJECT_ROOT}")
target_link_libraries(test_temporal_stress PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- Audit proof tests (P1/P2 finding verification) ----
add_executable(test_audit_proof
    test_audit_proof.cpp
    ${CPP_ROOT}/common/ClipIndex.cpp
)
target_link_libraries(test_audit_proof PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- Ingest thread tests (ring buffer + thread lifecycle + SPSC) ----
add_executable(test_ingest_thread
    test_ingest_thread.cpp
    ${CPP_ROOT}/common/ClipIndex.cpp
    ${CPP_ROOT}/demux/WebmDemuxer.cpp
    ${CPP_ROOT}/demux/WebmTrackParser.cpp
    ${CPP_ROOT}/demux/WebmBlockParser.cpp
    ${LIBWEBM_ROOT}/mkvparser/mkvparser.cc
)
target_include_directories(test_ingest_thread PRIVATE ${LIBWEBM_ROOT})
target_link_libraries(test_ingest_thread PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- Transcript module tests (ring buffer + registry) ----
add_executable(test_transcript
    test_transcript.cpp
)
target_include_directories(test_transcript PRIVATE
    ${CPP_ROOT}/transcript
)

# ---- RT safety test (malloc interposition, no sanitizer) ----
# Detects heap allocations during readSamples() via interposed malloc.
# Must build without sanitizer — ASan replaces malloc, breaking interposition.
add_executable(test_rt_safety
    test_rt_safety.cpp
)
target_link_libraries(test_rt_safety PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- HealthWatchdog state machine test ----
add_executable(test_health_watchdog
    test_health_watchdog.cpp
)

# ---- V1 audit fix regression tests ----
add_executable(test_v1_fixes
    test_v1_fixes.cpp
)
target_link_libraries(test_v1_fixes PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

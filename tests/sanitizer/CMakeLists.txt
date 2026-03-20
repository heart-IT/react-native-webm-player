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
else()
    message(FATAL_ERROR "Unknown SANITIZER=${SANITIZER}. Use 'address', 'thread', or 'undefined'.")
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

# ---- LSAN test (leak + memory tests) ----
add_executable(test_lsan
    test_lsan.cpp
)
target_link_libraries(test_lsan PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- TSAN test (threading tests) ----
add_executable(test_tsan
    test_tsan.cpp
)
target_link_libraries(test_tsan PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- UBSAN test (undefined behavior) ----
add_executable(test_ubsan
    test_ubsan.cpp
)
target_link_libraries(test_ubsan PRIVATE ${OPUS_LIBRARY} ${SPEEXDSP_LIBRARY})

# ---- Route handler test (pure C++ reactive routing, no Opus/DSP dependencies) ----
add_executable(test_route_handler
    test_route_handler.cpp
)

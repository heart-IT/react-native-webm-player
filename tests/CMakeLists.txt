cmake_minimum_required(VERSION 3.20)
project(webm_player_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(CPP_ROOT "${PROJECT_ROOT}/cpp")

# The shared C++ compiles on the host via MEDIA_PLATFORM_HOST (see Platform.h).
# No -DTARGET_OS_IOS fiction required.

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# -DSANITIZER=address|thread|undefined|none
if(NOT DEFINED SANITIZER)
    set(SANITIZER "address" CACHE STRING "address, thread, undefined, none")
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
    message(STATUS "Building without sanitizer")
    add_compile_options(-g -O0)
else()
    message(FATAL_ERROR "Unknown SANITIZER=${SANITIZER}")
endif()

include_directories(
    ${CPP_ROOT}
    ${CPP_ROOT}/common
    ${CPP_ROOT}/demux
    ${CPP_ROOT}/playback
    ${CPP_ROOT}/video
    ${CPP_ROOT}/third_party/libwebm
)

find_package(Threads REQUIRED)

# ---- Shared byte ring ----
add_executable(test_stream_buffer
    test_stream_buffer.cpp
    ${CPP_ROOT}/common/WebMStreamBuffer.cpp
)
target_link_libraries(test_stream_buffer PRIVATE Threads::Threads)

# ---- Audit proof tests ----
# These assert the behaviour the system is supposed to have, so they fail
# against unfixed code. Verdicts recorded in docs/AUDIT.md.
add_executable(test_audit_proof
    test_audit_proof.cpp
    ${CPP_ROOT}/common/WebMStreamBuffer.cpp
)
target_link_libraries(test_audit_proof PRIVATE Threads::Threads)

# Separate binary: ASan aborts on first error, which would mask later tests.
add_executable(test_ring_lifetime
    test_ring_lifetime.cpp
    ${CPP_ROOT}/common/WebMStreamBuffer.cpp
)
target_link_libraries(test_ring_lifetime PRIVATE Threads::Threads)

# ---- WebM demuxer end-to-end (real VP9+Opus fixture) ----
# Opus is only needed to prove the demuxed audio packets actually decode.
find_path(OPUS_INCLUDE_DIR opus/opus.h HINTS /opt/homebrew/include /usr/local/include)
find_library(OPUS_LIBRARY opus HINTS /opt/homebrew/lib /usr/local/lib)

if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
    add_executable(test_e2e_demux
        test_e2e_demux.cpp
        ${CPP_ROOT}/demux/WebmDemuxer.cpp
        ${CPP_ROOT}/demux/WebmTrackParser.cpp
        ${CPP_ROOT}/demux/WebmBlockParser.cpp
        ${CPP_ROOT}/third_party/libwebm/mkvparser/mkvparser.cc
    )
    target_include_directories(test_e2e_demux PRIVATE ${OPUS_INCLUDE_DIR}/opus)
    target_link_libraries(test_e2e_demux PRIVATE ${OPUS_LIBRARY} Threads::Threads)
else()
    message(WARNING "Opus not found (brew install opus) — skipping test_e2e_demux")
endif()

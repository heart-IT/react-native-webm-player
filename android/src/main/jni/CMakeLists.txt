cmake_minimum_required(VERSION 3.13)
project(webmplayer)

# AAudio requires Android API 26+, we target API 29 for full feature support
if(ANDROID_NATIVE_API_LEVEL LESS 29)
    message(WARNING "webm-player requires minSdkVersion >= 29 for AAudio support. "
                    "Current ANDROID_NATIVE_API_LEVEL=${ANDROID_NATIVE_API_LEVEL}. "
                    "Please set minSdkVersion to 29 or higher in your app's build.gradle.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${REACT_ANDROID_DIR}/cmake-utils/ReactNative-application.cmake)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Don't update fetched content after first download")

# Opus 1.6.1 — audio decoder
# Release tarball includes pre-generated DNN model data files required by OSCE.
set(OPUS_TARBALL_URL "https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz")
set(OPUS_TARBALL_SHA256 "6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1")
set(OPUS_TARBALL_DIR "${CMAKE_BINARY_DIR}/_deps")
set(OPUS_TARBALL "${OPUS_TARBALL_DIR}/opus-1.6.1.tar.gz")
set(OPUS_SOURCE_DIR "${OPUS_TARBALL_DIR}/opus-src")
if(NOT EXISTS "${OPUS_SOURCE_DIR}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${OPUS_TARBALL_DIR}")
    if(NOT EXISTS "${OPUS_TARBALL}")
        message(STATUS "Downloading Opus 1.6.1 release tarball (includes OSCE data)...")
        execute_process(
            COMMAND curl -sL -o "${OPUS_TARBALL}" "${OPUS_TARBALL_URL}"
            RESULT_VARIABLE OPUS_DL_RESULT
        )
        if(NOT OPUS_DL_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to download Opus tarball (exit ${OPUS_DL_RESULT})")
        endif()
        file(SHA256 "${OPUS_TARBALL}" OPUS_ACTUAL_SHA256)
        if(NOT "${OPUS_ACTUAL_SHA256}" STREQUAL "${OPUS_TARBALL_SHA256}")
            file(REMOVE "${OPUS_TARBALL}")
            message(FATAL_ERROR "Opus tarball SHA256 mismatch: ${OPUS_ACTUAL_SHA256}")
        endif()
    endif()
    message(STATUS "Extracting Opus 1.6.1...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${OPUS_TARBALL}"
        WORKING_DIRECTORY "${OPUS_TARBALL_DIR}"
    )
    file(RENAME "${OPUS_TARBALL_DIR}/opus-1.6.1" "${OPUS_SOURCE_DIR}")
endif()
FetchContent_Declare(opus SOURCE_DIR "${OPUS_SOURCE_DIR}")
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

# libwebm — WebM container demuxer
FetchContent_Declare(
    libwebm
    GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebm.git
    GIT_TAG libwebm-1.0.0.31
    GIT_SHALLOW TRUE
)
set(ENABLE_WEBMTS OFF CACHE BOOL "" FORCE)
set(ENABLE_WEBM_PARSER OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SAMPLE_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libwebm)

find_library(log-lib log)
find_library(android-lib android)

# Shared cross-platform code location
set(CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../cpp)

# JSI installation sources (Android-specific)
set(JSI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/jsi_installer_jni.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/JSIInstaller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mediapipeline_jni.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audiosession_jni.cpp
)

# Playback/RX path (decoder) - Android-specific implementation
set(PLAYBACK_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/playback/MediaPipelineModule.cpp
)

# Video pipeline (decode + view) — no capture
set(VIDEO_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/video/VideoPipelineModule.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/MediaCodecDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/VideoDecodeThread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/videopipeline_jni.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/videoview_jni.cpp
)

# SpeexDSP resampler — vendored for drift compensation
set(SPEEXDSP_ROOT ${CPP_ROOT}/third_party/speexdsp)
set(SPEEXDSP_SOURCES
    ${SPEEXDSP_ROOT}/libspeexdsp/resample.c
)

# WebM demuxer — incremental streaming parser using libwebm mkvparser
set(DEMUX_SOURCES
    ${CPP_ROOT}/demux/WebmDemuxer.cpp
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${JSI_SOURCES}
    ${PLAYBACK_SOURCES}
    ${VIDEO_SOURCES}
    ${DEMUX_SOURCES}
    ${SPEEXDSP_SOURCES}
)

target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC
    # Shared cross-platform headers
    ${CPP_ROOT}
    ${CPP_ROOT}/common
    ${CPP_ROOT}/playback
    ${CPP_ROOT}/video
    ${CPP_ROOT}/demux
    # Android-specific headers
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/playback
    ${CMAKE_CURRENT_SOURCE_DIR}/video
    # Opus headers
    ${opus_SOURCE_DIR}/include
    # libwebm headers
    ${libwebm_SOURCE_DIR}
    # SpeexDSP headers
    ${SPEEXDSP_ROOT}/include
    ${SPEEXDSP_ROOT}/libspeexdsp
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_OPUS=1 FLOATING_POINT=1 EXPORT=)

if(NATIVE_RELEASE OR CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NDEBUG)

    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        -O3
        -funroll-loops
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -ffunction-sections
        -fdata-sections
        -flto=thin
    )

    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE _FORTIFY_SOURCE=2)

    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        -flto=thin
        -Wl,--gc-sections
        -Wl,-z,relro,-z,now
    )
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        MEDIA_DEBUG
        MEDIA_DEBUG_THREADING
    )
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -O1 -g -fno-omit-frame-pointer)
endif()

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wconversion
    -Wshadow
    -Wno-unused-parameter
    -Werror=return-type
    -Werror=non-virtual-dtor
    -Werror=uninitialized
    -Werror=format-security
    -Werror=implicit-fallthrough
    -fstack-protector-strong
)

# arm64-v8a only (no armeabi-v7a support)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -march=armv8-a+simd)

target_compile_options(opus PRIVATE -O3)

if(NATIVE_RELEASE OR CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(opus PRIVATE -flto=thin)
    target_link_options(opus PRIVATE -flto=thin)
endif()

target_link_libraries(${CMAKE_PROJECT_NAME}
    fbjni
    reactnative
    ${log-lib}
    ${android-lib}
    aaudio
    mediandk
    opus
    webm
)

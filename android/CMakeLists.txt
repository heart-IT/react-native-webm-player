project(WebmPlayer)
cmake_minimum_required(VERSION 3.9.0)

set (PACKAGE_NAME WebmPlayer)
set (CMAKE_VERBOSE_MAKEFILE ON)
set (CMAKE_CXX_STANDARD 20)

# Enable Raw Props parsing in react-native (for Nitro Views)
add_compile_options(-DRN_SERIALIZABLE_STATE=1)

# Define C++ library and add all sources.
# src/main/cpp/cpp-adapter.cpp is the fbjni entry point for the Android
# HybridObject; it is compiled back in alongside the ExoPlayer implementation.
# Until then this library exists to build the shared C++ for every Android ABI.
add_library(${PACKAGE_NAME} SHARED
        ../cpp/common/WebMStreamBuffer.cpp
)

# Nitrogen's Android autolinking is generated only when the spec declares
# `android: 'kotlin'`. The WebmPlayer HybridObject is iOS-only until the
# ExoPlayer implementation lands; restore this include with it.
# include(${CMAKE_SOURCE_DIR}/../nitrogen/generated/android/WebmPlayer+autolinking.cmake)

# Set up local includes
include_directories(
        "src/main/cpp"
        "../cpp"
)

find_library(LOG_LIB log)

# Link all libraries together
target_link_libraries(
        ${PACKAGE_NAME}
        ${LOG_LIB}
        android                                   # <-- Android core
)

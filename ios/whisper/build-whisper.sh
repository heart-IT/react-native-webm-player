#!/bin/bash
# Build whisper.cpp as an XCFramework for iOS (arm64 device + arm64 simulator).
# Usage: ./build-whisper.sh [clean|build]
#
# Output: ios/whisper/lib/whisper.xcframework
# The XCFramework includes both the whisper and ggml static libraries.

set -euo pipefail

WHISPER_VERSION="v1.8.4"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_DIR="${SCRIPT_DIR}/lib"
INCLUDE_DIR="${OUTPUT_DIR}/include"

clean() {
    echo "Cleaning whisper build artifacts..."
    rm -rf "${BUILD_DIR}"
    rm -rf "${OUTPUT_DIR}"
    echo "Done."
}

build() {
    # Clone if needed
    local SRC_DIR="${BUILD_DIR}/whisper-src"
    if [ ! -d "${SRC_DIR}" ]; then
        echo "Cloning whisper.cpp ${WHISPER_VERSION}..."
        git clone --depth 1 --branch "${WHISPER_VERSION}" \
            https://github.com/ggml-org/whisper.cpp.git "${SRC_DIR}"
    fi

    local IOS_BUILD="${BUILD_DIR}/ios-arm64"
    local SIM_ARM64_BUILD="${BUILD_DIR}/sim-arm64"
    local SIM_X86_BUILD="${BUILD_DIR}/sim-x86_64"

    # Common CMake flags
    local COMMON_FLAGS=(
        -DCMAKE_BUILD_TYPE=Release
        -DWHISPER_BUILD_TESTS=OFF
        -DWHISPER_BUILD_EXAMPLES=OFF
        -DWHISPER_BUILD_SERVER=OFF
        -DWHISPER_SDL2=OFF
        -DWHISPER_CURL=OFF
        -DWHISPER_COREML=OFF
        -DGGML_METAL=ON
        -DGGML_METAL_EMBED_LIBRARY=ON
        -DGGML_OPENMP=OFF
        -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_CXX_STANDARD=20
    )

    # Build for iOS device (arm64)
    echo "Building whisper.cpp for iOS arm64..."
    cmake -S "${SRC_DIR}" -B "${IOS_BUILD}" \
        "${COMMON_FLAGS[@]}" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=15.1 \
        -DCMAKE_INSTALL_PREFIX="${IOS_BUILD}/install"
    cmake --build "${IOS_BUILD}" --config Release -j "$(sysctl -n hw.ncpu)"

    # Build for iOS simulator (arm64)
    echo "Building whisper.cpp for iOS Simulator arm64..."
    cmake -S "${SRC_DIR}" -B "${SIM_ARM64_BUILD}" \
        "${COMMON_FLAGS[@]}" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT=iphonesimulator \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=15.1 \
        -DCMAKE_INSTALL_PREFIX="${SIM_ARM64_BUILD}/install"
    cmake --build "${SIM_ARM64_BUILD}" --config Release -j "$(sysctl -n hw.ncpu)"

    # Build for iOS simulator (x86_64) — required so the xcframework's simulator
    # slice is named ios-arm64_x86_64-simulator. CocoaPods derives the slice name
    # from the consumer app's $ARCHS at build time; if Xcode targets both archs
    # for simulator the universal name is required, otherwise [CP] Copy
    # XCFrameworks fails to find the slice.
    echo "Building whisper.cpp for iOS Simulator x86_64..."
    cmake -S "${SRC_DIR}" -B "${SIM_X86_BUILD}" \
        "${COMMON_FLAGS[@]}" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        -DCMAKE_OSX_SYSROOT=iphonesimulator \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=15.1 \
        -DCMAKE_INSTALL_PREFIX="${SIM_X86_BUILD}/install"
    cmake --build "${SIM_X86_BUILD}" --config Release -j "$(sysctl -n hw.ncpu)"

    # Create fat static libraries (whisper + ggml combined)
    echo "Creating combined static libraries..."
    mkdir -p "${BUILD_DIR}/ios-combined" "${BUILD_DIR}/sim-arm64-combined" \
             "${BUILD_DIR}/sim-x86_64-combined" "${BUILD_DIR}/sim-combined"

    libtool -static -o "${BUILD_DIR}/ios-combined/libwhisper.a" \
        "${IOS_BUILD}/src/libwhisper.a" \
        "${IOS_BUILD}/ggml/src/libggml.a" \
        "${IOS_BUILD}/ggml/src/ggml-cpu/libggml-cpu.a" \
        "${IOS_BUILD}/ggml/src/ggml-metal/libggml-metal.a" 2>/dev/null || \
    libtool -static -o "${BUILD_DIR}/ios-combined/libwhisper.a" \
        "${IOS_BUILD}/src/libwhisper.a" \
        $(find "${IOS_BUILD}/ggml" -name "*.a" -type f)

    libtool -static -o "${BUILD_DIR}/sim-arm64-combined/libwhisper.a" \
        "${SIM_ARM64_BUILD}/src/libwhisper.a" \
        $(find "${SIM_ARM64_BUILD}/ggml" -name "*.a" -type f)

    libtool -static -o "${BUILD_DIR}/sim-x86_64-combined/libwhisper.a" \
        "${SIM_X86_BUILD}/src/libwhisper.a" \
        $(find "${SIM_X86_BUILD}/ggml" -name "*.a" -type f)

    # Lipo the per-arch sim libs into a fat binary so the xcframework slice is
    # named ios-arm64_x86_64-simulator (matches CocoaPods' expectation).
    lipo -create \
        "${BUILD_DIR}/sim-arm64-combined/libwhisper.a" \
        "${BUILD_DIR}/sim-x86_64-combined/libwhisper.a" \
        -output "${BUILD_DIR}/sim-combined/libwhisper.a"

    # Create XCFramework
    echo "Creating whisper.xcframework..."
    mkdir -p "${OUTPUT_DIR}"
    rm -rf "${OUTPUT_DIR}/whisper.xcframework"

    xcodebuild -create-xcframework \
        -library "${BUILD_DIR}/ios-combined/libwhisper.a" \
        -headers "${SRC_DIR}/include" \
        -library "${BUILD_DIR}/sim-combined/libwhisper.a" \
        -headers "${SRC_DIR}/include" \
        -output "${OUTPUT_DIR}/whisper.xcframework"

    # Copy whisper + ggml headers (whisper.h includes ggml.h and ggml-cpu.h)
    mkdir -p "${INCLUDE_DIR}"
    cp "${SRC_DIR}/include/whisper.h" "${INCLUDE_DIR}/"
    cp "${SRC_DIR}/ggml/include/ggml.h" "${INCLUDE_DIR}/" 2>/dev/null || true
    cp "${SRC_DIR}/ggml/include/ggml-cpu.h" "${INCLUDE_DIR}/" 2>/dev/null || true
    cp "${SRC_DIR}/ggml/include/ggml-backend.h" "${INCLUDE_DIR}/" 2>/dev/null || true
    cp "${SRC_DIR}/ggml/include/ggml-alloc.h" "${INCLUDE_DIR}/" 2>/dev/null || true

    echo ""
    echo "Done! XCFramework created at: ${OUTPUT_DIR}/whisper.xcframework"
    echo ""
    echo "To use, add to your podspec:"
    echo '  s.vendored_frameworks = [..., "ios/whisper/lib/whisper.xcframework"]'
}

case "${1:-build}" in
    clean) clean ;;
    build) build ;;
    *) echo "Usage: $0 [clean|build]"; exit 1 ;;
esac

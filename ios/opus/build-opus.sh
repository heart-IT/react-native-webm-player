#!/bin/bash
# Build Opus 1.6.1 as an XCFramework for iOS using CMake.
# Usage: ./build-opus.sh [clean|build]
#
# Output: ios/opus/lib/opus.xcframework + ios/opus/lib/include/opus/
set -euo pipefail

OPUS_VERSION="1.6.1"
OPUS_SHA256="6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_DIR="${SCRIPT_DIR}/lib"

MIN_IOS_VERSION="15.1"

download_opus() {
    local tarball="${BUILD_DIR}/opus-${OPUS_VERSION}.tar.gz"

    if [ -f "$tarball" ]; then
        echo "Opus source already downloaded"
        return
    fi

    mkdir -p "$BUILD_DIR"
    echo "Downloading Opus ${OPUS_VERSION}..."
    curl -sL "https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz" -o "$tarball"

    echo "Verifying checksum..."
    local actual_sha256=$(shasum -a 256 "$tarball" | awk '{print $1}')
    if [ "$actual_sha256" != "$OPUS_SHA256" ]; then
        echo "Checksum mismatch! Expected: $OPUS_SHA256, Got: $actual_sha256"
        rm "$tarball"
        exit 1
    fi

    echo "Extracting..."
    tar -xzf "$tarball" -C "$BUILD_DIR"
}

build_arch() {
    local arch=$1
    local sysroot=$2
    local build_subdir="${BUILD_DIR}/build-${sysroot}-${arch}"
    local src_dir="${BUILD_DIR}/opus-${OPUS_VERSION}"

    echo "Building Opus for ${sysroot} ${arch} (CMake)..."

    local system_name="iOS"
    local sysroot_flag=""
    if [ "$sysroot" = "iphonesimulator" ]; then
        sysroot_flag="-DCMAKE_OSX_SYSROOT=iphonesimulator"
    fi

    cmake -S "$src_dir" -B "$build_subdir" \
        -DCMAKE_SYSTEM_NAME="$system_name" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS_VERSION" \
        ${sysroot_flag} \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPUS_BUILD_PROGRAMS=OFF \
        -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF \
        -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF \
        -DOPUS_CUSTOM_MODES=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_C_FLAGS="-O3 -DNDEBUG"

    cmake --build "$build_subdir" --config Release -j "$(sysctl -n hw.ncpu)"
}

create_xcframework() {
    echo "Creating XCFramework..."
    mkdir -p "$OUTPUT_DIR"

    local src_dir="${BUILD_DIR}/opus-${OPUS_VERSION}"

    # Simulator may have multiple archs — create fat lib
    local sim_libs=""
    for arch in arm64 x86_64; do
        local lib="${BUILD_DIR}/build-iphonesimulator-${arch}/libopus.a"
        if [ -f "$lib" ]; then
            sim_libs="${sim_libs} ${lib}"
        fi
    done
    # CocoaPods requires the same binary name across xcframework slices, so the
    # simulator fat lib is placed in its own dir and keeps the libopus.a name.
    local sim_dir="${BUILD_DIR}/sim-fat"
    mkdir -p "$sim_dir"
    local sim_fat="${sim_dir}/libopus.a"
    lipo -create $sim_libs -output "$sim_fat"

    local ios_lib="${BUILD_DIR}/build-iphoneos-arm64/libopus.a"

    # Copy headers
    local headers_dir="${OUTPUT_DIR}/include/opus"
    mkdir -p "$headers_dir"
    cp "${src_dir}/include/"*.h "$headers_dir/"

    rm -rf "${OUTPUT_DIR}/opus.xcframework"
    xcodebuild -create-xcframework \
        -library "$ios_lib" -headers "${OUTPUT_DIR}/include" \
        -library "$sim_fat" -headers "${OUTPUT_DIR}/include" \
        -output "${OUTPUT_DIR}/opus.xcframework"

    echo "XCFramework created at ${OUTPUT_DIR}/opus.xcframework"
}

clean() {
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
}

build() {
    download_opus
    build_arch arm64 iphoneos
    build_arch arm64 iphonesimulator
    build_arch x86_64 iphonesimulator
    create_xcframework
    echo "Done!"
}

case "${1:-build}" in
    clean) clean ;;
    build) build ;;
    *) echo "Usage: $0 [clean|build]"; exit 1 ;;
esac

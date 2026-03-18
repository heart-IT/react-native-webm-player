#!/bin/bash
set -e

OPUS_VERSION="1.6.1"
OPUS_SHA256="6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_DIR="${SCRIPT_DIR}/lib"

MIN_IOS_VERSION="15.1"

ARCHS_IOS="arm64"
ARCHS_SIM="arm64 x86_64"

download_opus() {
    local tarball="${BUILD_DIR}/opus-${OPUS_VERSION}.tar.gz"

    if [ -f "$tarball" ]; then
        echo "Opus source already downloaded"
        return
    fi

    mkdir -p "$BUILD_DIR"
    echo "Downloading Opus ${OPUS_VERSION}..."
    curl -L "https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz" -o "$tarball"

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
    local platform=$2
    local sdk=$3

    local src_dir="${BUILD_DIR}/opus-${OPUS_VERSION}"
    local build_subdir="${BUILD_DIR}/build-${platform}-${arch}"
    local prefix="${BUILD_DIR}/install-${platform}-${arch}"

    echo "Building Opus for ${platform} ${arch}..."

    mkdir -p "$build_subdir"
    cd "$build_subdir"

    local sdk_path=$(xcrun --sdk $sdk --show-sdk-path)
    local cc=$(xcrun --sdk $sdk --find clang)

    local cflags="-arch ${arch} -isysroot ${sdk_path} -O3 -DNDEBUG"
    local ldflags="-arch ${arch} -isysroot ${sdk_path}"

    if [ "$platform" = "iphoneos" ]; then
        cflags="${cflags} -miphoneos-version-min=${MIN_IOS_VERSION}"
        ldflags="${ldflags} -miphoneos-version-min=${MIN_IOS_VERSION}"
        host="${arch}-apple-darwin"
    else
        cflags="${cflags} -mios-simulator-version-min=${MIN_IOS_VERSION}"
        ldflags="${ldflags} -mios-simulator-version-min=${MIN_IOS_VERSION}"
        if [ "$arch" = "arm64" ]; then
            host="aarch64-apple-darwin"
        else
            host="x86_64-apple-darwin"
        fi
    fi

    CC="$cc" \
    CFLAGS="$cflags" \
    LDFLAGS="$ldflags" \
    "${src_dir}/configure" \
        --host="$host" \
        --prefix="$prefix" \
        --disable-shared \
        --enable-static \
        --disable-doc \
        --disable-extra-programs \
        --enable-custom-modes \
        --enable-osce

    make -j$(sysctl -n hw.ncpu)
    make install

    cd "$SCRIPT_DIR"
}

create_xcframework() {
    echo "Creating XCFramework..."

    local ios_libs=""
    local sim_libs=""

    for arch in $ARCHS_IOS; do
        ios_libs="${ios_libs} ${BUILD_DIR}/install-iphoneos-${arch}/lib/libopus.a"
    done

    for arch in $ARCHS_SIM; do
        sim_libs="${sim_libs} ${BUILD_DIR}/install-iphonesimulator-${arch}/lib/libopus.a"
    done

    mkdir -p "$OUTPUT_DIR"

    # Create fat library for simulator (multiple archs) with same name as device
    local sim_fat="${BUILD_DIR}/libopus.a"
    lipo -create $sim_libs -output "$sim_fat"

    # iOS device library
    local ios_lib="${BUILD_DIR}/install-iphoneos-arm64/lib/libopus.a"

    # Copy headers
    local headers_dir="${OUTPUT_DIR}/include"
    mkdir -p "$headers_dir"
    cp -R "${BUILD_DIR}/install-iphoneos-arm64/include/opus" "$headers_dir/"

    # Create XCFramework with consistent library names
    rm -rf "${OUTPUT_DIR}/opus.xcframework"
    xcodebuild -create-xcframework \
        -library "$ios_lib" -headers "$headers_dir" \
        -library "$sim_fat" -headers "$headers_dir" \
        -output "${OUTPUT_DIR}/opus.xcframework"

    echo "XCFramework created at ${OUTPUT_DIR}/opus.xcframework"
}

clean() {
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
}

main() {
    case "${1:-build}" in
        clean)
            clean
            ;;
        download)
            download_opus
            ;;
        build)
            download_opus

            for arch in $ARCHS_IOS; do
                build_arch "$arch" "iphoneos" "iphoneos"
            done

            for arch in $ARCHS_SIM; do
                build_arch "$arch" "iphonesimulator" "iphonesimulator"
            done

            create_xcframework
            echo "Done!"
            ;;
        *)
            echo "Usage: $0 [build|clean|download]"
            exit 1
            ;;
    esac
}

main "$@"

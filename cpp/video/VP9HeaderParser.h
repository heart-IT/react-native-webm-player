#pragma once

#include <cstdint>
#include <cstddef>

namespace media {

// Shared VP9 uncompressed header parser. Extracts frame dimensions from keyframes.
// Used by both iOS (VTDecompressionSession) and Android (MediaCodec) to get accurate
// initial dimensions without waiting for decoder format change events.
struct VP9FrameInfo {
    int width = 0;
    int height = 0;
    int profile = 0;
    bool isKeyFrame = false;
    bool valid = false;
};

namespace vp9 {

namespace detail {

// Minimal bitstream reader for VP9 uncompressed header parsing
class BitstreamReader {
public:
    BitstreamReader(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    uint32_t readBits(int n) {
        if (n <= 0 || n > 32) { error_ = (n != 0); return 0; }
        uint32_t val = 0;
        for (int i = 0; i < n; ++i) {
            if (byteOffset_ >= size_) { error_ = true; return 0; }
            uint32_t bit = (data_[byteOffset_] >> (7 - bitOffset_)) & 1;
            val = (val << 1) | bit;
            if (++bitOffset_ == 8) {
                bitOffset_ = 0;
                ++byteOffset_;
            }
        }
        return val;
    }

    bool hasError() const { return error_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t byteOffset_ = 0;
    int bitOffset_ = 0;
    bool error_ = false;
};

}  // namespace detail

// Parse VP9 uncompressed header to extract frame info.
// For keyframes, extracts width and height. For non-keyframes, only sets isKeyFrame=false.
// Returns info with valid=false on parse error or insufficient data.
inline VP9FrameInfo parseHeader(const uint8_t* data, size_t size) {
    VP9FrameInfo info{};
    if (!data || size < 4) return info;

    detail::BitstreamReader reader(data, size);

    // frame_marker: 2 bits, must be 0b10
    uint32_t marker = reader.readBits(2);
    if (reader.hasError() || marker != 0x2) return info;

    // profile
    uint32_t profileLow = reader.readBits(1);
    uint32_t profileHigh = reader.readBits(1);
    info.profile = static_cast<int>((profileHigh << 1) | profileLow);

    if (info.profile == 3) {
        reader.readBits(1); // reserved_zero
    }

    // show_existing_frame
    uint32_t showExisting = reader.readBits(1);
    if (showExisting) return info;

    // frame_type: 0 = keyframe
    uint32_t frameType = reader.readBits(1);
    info.isKeyFrame = (frameType == 0);

    // show_frame
    reader.readBits(1);

    // error_resilient_mode
    reader.readBits(1);

    if (reader.hasError()) return info;

    if (!info.isKeyFrame) {
        info.valid = true;
        return info;
    }

    // Keyframe: frame_sync_code (24 bits = 0x49 0x83 0x42)
    uint32_t sync1 = reader.readBits(8);
    uint32_t sync2 = reader.readBits(8);
    uint32_t sync3 = reader.readBits(8);
    if (reader.hasError() || sync1 != 0x49 || sync2 != 0x83 || sync3 != 0x42) return info;

    // Color config
    if (info.profile >= 2) {
        reader.readBits(1); // ten_or_twelve_bit
    }

    uint32_t colorSpace = reader.readBits(3);
    if (reader.hasError()) return info;

    if (colorSpace != 7) { // not CS_RGB
        reader.readBits(1); // color_range
        if (info.profile == 1 || info.profile == 3) {
            reader.readBits(1); // subsampling_x
            reader.readBits(1); // subsampling_y
            reader.readBits(1); // reserved
        }
    } else {
        // CS_RGB: color_range is implicitly 1
        if (info.profile == 1 || info.profile == 3) {
            reader.readBits(1); // reserved
        }
    }

    if (reader.hasError()) return info;

    // frame_width_minus1: 16 bits
    uint32_t widthMinus1 = reader.readBits(16);
    // frame_height_minus1: 16 bits
    uint32_t heightMinus1 = reader.readBits(16);

    if (reader.hasError()) return info;

    info.width = static_cast<int>(widthMinus1 + 1);
    info.height = static_cast<int>(heightMinus1 + 1);
    info.valid = true;
    return info;
}

}  // namespace vp9
}  // namespace media

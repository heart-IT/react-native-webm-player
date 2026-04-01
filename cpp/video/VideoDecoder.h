#pragma once

#include <cstdint>
#include <cstddef>

namespace media {

// Abstract interface for platform video decoders (MediaCodec on Android, VTDecompress on iOS).
// Used by VideoDecodeThread to decouple the shared decode loop from platform decoder details.
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    // Submit an encoded VP9 frame for decoding. Returns true on success.
    virtual bool submitFrame(const uint8_t* data, size_t size,
                             int64_t ptsUs, bool isKeyFrame) = 0;

    // Called after each decode loop iteration. Android uses this to release
    // MediaCodec output buffers to the surface. Default is no-op.
    virtual void postDecode() {}

    // Decoded frame dimensions (0 if not yet known).
    virtual int decodedWidth() const = 0;
    virtual int decodedHeight() const = 0;

    // Last platform error code (media_status_t on Android, OSStatus on iOS).
    virtual int lastError() const { return 0; }

    // Clean shutdown.
    virtual void shutdown() = 0;
};

}  // namespace media

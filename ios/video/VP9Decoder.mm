#include "VP9Decoder.h"
#include "../../cpp/common/MediaLog.h"

namespace videomodule {

namespace {

// Minimal bitstream reader for VP9 uncompressed header parsing
class BitstreamReader {
public:
    BitstreamReader(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    uint32_t readBits(int n) {
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
    size_t bitsConsumed() const { return byteOffset_ * 8 + bitOffset_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t byteOffset_ = 0;
    int bitOffset_ = 0;
    bool error_ = false;
};

}  // namespace

VP9Decoder::VP9FrameInfo VP9Decoder::parseVP9Header(const uint8_t* data, size_t size) {
    VP9FrameInfo info{};
    if (!data || size < 4) return info;

    BitstreamReader reader(data, size);

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

void VP9Decoder::setOutputLayer(AVSampleBufferDisplayLayer* layer) {
    if (layer_ == layer) return;
    shutdown();
    if (layer) {
        layer_ = layer;
        [layer_ retain];
        renderQueue_ = dispatch_queue_create("com.holepunchto.vp9.render", DISPATCH_QUEUE_SERIAL);

        // VP9 FourCC 'vp09' — use raw value to avoid iOS 16+ SDK constant
        // requirement. VTIsHardwareDecodeSupported is available from iOS 11;
        // VP9 HW decode is available on A12+ chips from iOS 14.
        constexpr CMVideoCodecType kVP9CodecType = 'vp09';
        hwSupported_ = VTIsHardwareDecodeSupported(kVP9CodecType);
        MEDIA_LOG_I("VP9Decoder: HW decode %s", hwSupported_ ? "supported" : "not supported");
    }
}

void VP9Decoder::shutdown() {
    destroyDecompressionSession();
    if (renderQueue_) {
        dispatch_sync(renderQueue_, ^{});  // drain pending work
        renderQueue_ = nil;
    }
    if (layer_) {
        [layer_ release];
        layer_ = nil;
    }
    decodedWidth_.store(0, std::memory_order_relaxed);
    decodedHeight_.store(0, std::memory_order_relaxed);
    sessionWidth_ = 0;
    sessionHeight_ = 0;
}

bool VP9Decoder::createDecompressionSession(int width, int height) {
    destroyDecompressionSession();

    if (!hwSupported_) {
        MEDIA_LOG_E("VP9Decoder: cannot create session, HW decode not supported");
        return false;
    }

    // Create format description for VP9 (raw FourCC, see setOutputLayer)
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    OSStatus status = CMVideoFormatDescriptionCreate(
        kCFAllocatorDefault,
        kVP9CodecType,
        width,
        height,
        nullptr,  // extensions
        &formatDesc_);

    if (status != noErr || !formatDesc_) {
        MEDIA_LOG_E("VP9Decoder: CMVideoFormatDescriptionCreate failed: %d", (int)status);
        formatDesc_ = nullptr;
        return false;
    }

    // Destination pixel buffer attributes
    NSDictionary* destAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };

    VTDecompressionOutputCallbackRecord callbackRecord;
    callbackRecord.decompressionOutputCallback = vtDecompressionCallback;
    callbackRecord.decompressionOutputRefCon = this;

    status = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        formatDesc_,
        nullptr,  // videoDecoderSpecification
        (__bridge CFDictionaryRef)destAttrs,
        &callbackRecord,
        &session_);

    if (status != noErr || !session_) {
        MEDIA_LOG_E("VP9Decoder: VTDecompressionSessionCreate failed: %d", (int)status);
        CFRelease(formatDesc_);
        formatDesc_ = nullptr;
        session_ = nullptr;
        return false;
    }

    sessionWidth_ = width;
    sessionHeight_ = height;
    MEDIA_LOG_I("VP9Decoder: session created %dx%d", width, height);
    return true;
}

void VP9Decoder::destroyDecompressionSession() {
    if (session_) {
        VTDecompressionSessionWaitForAsynchronousFrames(session_);
        VTDecompressionSessionInvalidate(session_);
        CFRelease(session_);
        session_ = nullptr;
    }
    // Drain render queue to ensure no pending blocks reference stale state
    if (renderQueue_) {
        dispatch_sync(renderQueue_, ^{});
    }
    if (formatDesc_) {
        CFRelease(formatDesc_);
        formatDesc_ = nullptr;
    }
}

bool VP9Decoder::submitFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyFrame) {
    if (!layer_ || !data || size == 0) return false;

    if (!hwSupported_) {
        MEDIA_LOG_E("VP9Decoder: HW decode not supported, cannot decode frame");
        return false;
    }

    // Parse header for keyframes to detect dimensions
    if (isKeyFrame) {
        VP9FrameInfo info = parseVP9Header(data, size);
        if (!info.valid || info.width <= 0 || info.height <= 0) {
            MEDIA_LOG_E("VP9Decoder: failed to parse VP9 keyframe header");
            if (keyFrameRequestFn_) keyFrameRequestFn_();
            return false;
        }

        // Recreate session if dimensions changed or session doesn't exist
        if (!session_ || info.width != sessionWidth_ || info.height != sessionHeight_) {
            if (!createDecompressionSession(info.width, info.height)) {
                return false;
            }
        }
    }

    if (!session_) {
        // No session yet and this is not a keyframe — request one
        if (keyFrameRequestFn_) keyFrameRequestFn_();
        return false;
    }

    // Wrap raw VP9 data in CMBlockBuffer
    CMBlockBufferRef blockBuffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        nullptr,          // memoryBlock (null = allocate)
        size,
        kCFAllocatorDefault,
        nullptr,          // customBlockSource
        0,                // offsetToData
        size,
        kCMBlockBufferAssureMemoryNowFlag,
        &blockBuffer);

    if (status != noErr || !blockBuffer) {
        MEDIA_LOG_E("VP9Decoder: CMBlockBufferCreate failed: %d", (int)status);
        if (blockBuffer) CFRelease(blockBuffer);
        return false;
    }

    status = CMBlockBufferReplaceDataBytes(data, blockBuffer, 0, size);
    if (status != noErr) {
        MEDIA_LOG_E("VP9Decoder: CMBlockBufferReplaceDataBytes failed: %d", (int)status);
        CFRelease(blockBuffer);
        return false;
    }

    // Create CMSampleBuffer
    CMSampleBufferRef sampleBuffer = nullptr;
    size_t sampleSize = size;

    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, fps_);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        blockBuffer,
        formatDesc_,
        1,              // numSamples
        1,              // numSampleTimingEntries
        &timing,
        1,              // numSampleSizeEntries
        &sampleSize,
        &sampleBuffer);

    CFRelease(blockBuffer);

    if (status != noErr || !sampleBuffer) {
        MEDIA_LOG_E("VP9Decoder: CMSampleBufferCreateReady failed: %d", (int)status);
        return false;
    }

    // Pack int64_t PTS directly into void* (both 8 bytes on 64-bit ARM)
    void* ptsContext = reinterpret_cast<void*>(static_cast<uintptr_t>(ptsUs));

    VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
    VTDecodeInfoFlags infoFlagsOut = 0;

    status = VTDecompressionSessionDecodeFrame(
        session_,
        sampleBuffer,
        flags,
        ptsContext,  // sourceFrameRefCon
        &infoFlagsOut);

    CFRelease(sampleBuffer);

    if (status != noErr) {
        ++consecutiveErrors_;
        MEDIA_LOG_E("VP9Decoder: VTDecompressionSessionDecodeFrame failed: %d (consecutive=%d)",
                    (int)status, consecutiveErrors_);
        if (status == kVTInvalidSessionErr || consecutiveErrors_ >= kMaxConsecutiveErrors) {
            int64_t now = media::nowUs();
            if (now - lastResetTimeUs_ < resetBackoffUs_) {
                return false;  // Within backoff window, just drop
            }
            MEDIA_LOG_W("VP9Decoder: resetting decoder after %d errors (backoff=%lldms)",
                        consecutiveErrors_, static_cast<long long>(resetBackoffUs_ / 1000));
            destroyDecompressionSession();
            consecutiveErrors_ = 0;
            lastResetTimeUs_ = now;
            resetBackoffUs_ = std::min(resetBackoffUs_ * 2, int64_t{2000000});
            ++resetsSinceSuccess_;
            if (videoMetrics_) videoMetrics_->decoderResets.fetch_add(1, std::memory_order_relaxed);
            if (keyFrameRequestFn_) keyFrameRequestFn_();
        }
        return false;
    }

    consecutiveErrors_ = 0;
    resetBackoffUs_ = 100000;
    resetsSinceSuccess_ = 0;
    return true;
}

void VP9Decoder::vtDecompressionCallback(void* decompressionOutputRefCon,
                                          void* sourceFrameRefCon,
                                          OSStatus status,
                                          VTDecodeInfoFlags infoFlags,
                                          CVImageBufferRef imageBuffer,
                                          CMTime presentationTimeStamp,
                                          CMTime presentationDuration) {
    auto* self = static_cast<VP9Decoder*>(decompressionOutputRefCon);
    int64_t ptsUs = static_cast<int64_t>(reinterpret_cast<uintptr_t>(sourceFrameRefCon));

    if (status != noErr) {
        MEDIA_LOG_W("VP9Decoder: decode callback error: %d", (int)status);
        return;
    }

    if (!imageBuffer) {
        MEDIA_LOG_W("VP9Decoder: decode callback received null imageBuffer");
        return;
    }

    if (infoFlags & kVTDecodeInfo_FrameDropped) {
        return;
    }

    self->outputCallback(imageBuffer, ptsUs);
}

void VP9Decoder::outputCallback(CVImageBufferRef imageBuffer, int64_t ptsUs) {
    if (!imageBuffer || !layer_) return;

    int w = static_cast<int>(CVPixelBufferGetWidth(imageBuffer));
    int h = static_cast<int>(CVPixelBufferGetHeight(imageBuffer));
    decodedWidth_.store(w, std::memory_order_relaxed);
    decodedHeight_.store(h, std::memory_order_relaxed);

    // Create format description from the decoded pixel buffer
    CMVideoFormatDescriptionRef outputFormatDesc = nullptr;
    OSStatus status = CMVideoFormatDescriptionCreateForImageBuffer(
        kCFAllocatorDefault, imageBuffer, &outputFormatDesc);
    if (status != noErr || !outputFormatDesc) {
        MEDIA_LOG_W("VP9Decoder: CMVideoFormatDescriptionCreateForImageBuffer failed: %d", (int)status);
        return;
    }

    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, fps_);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sampleBuffer = nullptr;
    status = CMSampleBufferCreateReadyWithImageBuffer(
        kCFAllocatorDefault, imageBuffer, outputFormatDesc, &timing, &sampleBuffer);
    CFRelease(outputFormatDesc);

    if (status != noErr || !sampleBuffer) {
        MEDIA_LOG_W("VP9Decoder: CMSampleBufferCreateReadyWithImageBuffer failed: %d", (int)status);
        return;
    }

    // Dispatch to render queue to serialize enqueue calls from VT internal threads.
    // Retain layer to prevent use-after-free if VP9Decoder is destroyed while
    // the block is queued.
    AVSampleBufferDisplayLayer* layer = layer_;
    [layer retain];
    CFRetain(sampleBuffer);
    dispatch_async(renderQueue_, ^{
        if ([layer isReadyForMoreMediaData]) {
            [layer enqueueSampleBuffer:sampleBuffer];
        }
        CFRelease(sampleBuffer);
        [layer release];
    });
    CFRelease(sampleBuffer);
}

}  // namespace videomodule

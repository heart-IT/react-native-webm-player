#include "VP9Decoder.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/video/VP9HeaderParser.h"

namespace videomodule {

// Queue-identity key so shutdown can avoid dispatch_sync self-deadlock when called
// from a block already running on renderQueue_.
static const void* kRenderQueueKey = &kRenderQueueKey;

VP9Decoder::VP9Decoder() {
    renderQueue_ = dispatch_queue_create("com.heartit.vp9.render", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(renderQueue_, kRenderQueueKey, this, nullptr);

    // VP9 FourCC 'vp09' — use raw value to avoid iOS 16+ SDK constant
    // requirement. VTIsHardwareDecodeSupported is available from iOS 11;
    // VP9 HW decode is available on A12+ chips from iOS 14.
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';

    // iOS 26.2 reclassified VP9 as a "supplemental" video decoder — Apple
    // ships it but doesn't enable it until the app explicitly opts in via
    // VTRegisterSupplementalVideoDecoderIfAvailable. Without this call,
    // VTIsHardwareDecodeSupported('vp09') returns false and
    // VTDecompressionSessionCreate fails with codec-not-found. Pre-26.2 iOS
    // (and all macOS 11+) did not require the opt-in, so VP9 just worked.
    // The function itself is API_AVAILABLE(ios(26.2), macos(11.0), ...).
    if (@available(iOS 26.2, macOS 11.0, tvOS 26.2, visionOS 26.2, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(kVP9CodecType);
    }

    hwSupported_ = VTIsHardwareDecodeSupported(kVP9CodecType);
    MEDIA_LOG_I("VP9Decoder: HW decode %s", hwSupported_ ? "supported" : "not supported");
}

static inline void drainRenderQueue(dispatch_queue_t q, void* self) {
    // Skip the drain if we're already on the queue — dispatch_sync would self-deadlock.
    if (!q) return;
    if (dispatch_get_specific(kRenderQueueKey) == self) return;
    dispatch_sync(q, ^{});
}

void VP9Decoder::setOutputLayer(AVSampleBufferDisplayLayer* layer) {
    if (layerRaw_.load(std::memory_order_relaxed) == (__bridge void*)layer) return;
    shutdown();
    if (layer) {
        void* retained = (void*)CFBridgingRetain(layer);  // +1 ref, ARC-safe
        layerRaw_.store(retained, std::memory_order_release);
    }
}

void VP9Decoder::shutdown() {
    destroyDecompressionSession();
    drainRenderQueue(renderQueue_, this);
    void* raw = layerRaw_.load(std::memory_order_acquire);
    if (raw) {
        CFBridgingRelease(raw);  // -1 ref, ARC-safe
        layerRaw_.store(nullptr, std::memory_order_release);
    }
    decodedWidth_.store(0, std::memory_order_relaxed);
    decodedHeight_.store(0, std::memory_order_relaxed);
    sessionWidth_ = 0;
    sessionHeight_ = 0;
    // Mirror Android: a rebound decoder (setOutputLayer → shutdown → new session)
    // starts with a fresh error ladder instead of inheriting poisoned backoff.
    consecutiveErrors_ = 0;
    resetBackoffUs_ = media::video_config::decoder_reset::kInitialBackoffUs;
    lastResetTimeUs_ = 0;
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
    // Serialize against submitFrame's VTDecompressionSessionDecodeFrame call.
    std::lock_guard<std::mutex> lk(sessionMutex_);
    if (session_) {
        // SAFETY-CRITICAL: WaitForAsynchronousFrames drains all pending VT callbacks
        // before we invalidate the session. Skipping this causes use-after-free in
        // vtDecompressionCallback (which captures 'this' via decompressionOutputRefCon).
        VTDecompressionSessionWaitForAsynchronousFrames(session_);
        VTDecompressionSessionInvalidate(session_);
        CFRelease(session_);
        session_ = nullptr;
    }
    // Drain render queue to ensure no pending blocks reference stale state.
    // drainRenderQueue skips the drain if called from on-queue (avoids self-deadlock).
    drainRenderQueue(renderQueue_, this);
    if (formatDesc_) {
        CFRelease(formatDesc_);
        formatDesc_ = nullptr;
    }
}

bool VP9Decoder::submitFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyFrame) {
    // @autoreleasepool: createDecompressionSession creates ObjC objects (NSDictionary)
    // which need an autorelease pool on the decode thread (non-main, no implicit pool).
    @autoreleasepool {

    if (!layerRaw_.load(std::memory_order_acquire) || !data || size == 0) return false;

    if (!hwSupported_) {
        MEDIA_LOG_E("VP9Decoder: HW decode not supported, cannot decode frame");
        return false;
    }

    // Parse header for keyframes to detect dimensions (shared VP9 parser)
    if (isKeyFrame) {
        auto info = media::vp9::parseHeader(data, size);
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

    // Pack int64_t PTS directly into void* (both 8 bytes on 64-bit ARM).
    // PTS values from WebM are non-negative; negative values would corrupt on round-trip.
    static_assert(sizeof(void*) >= sizeof(int64_t), "void* must be at least 64-bit for PTS packing");
    void* ptsContext = reinterpret_cast<void*>(static_cast<uintptr_t>(ptsUs));

    VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
    VTDecodeInfoFlags infoFlagsOut = 0;

    // Hold sessionMutex_ for the duration of the VT call. destroyDecompressionSession
    // waits behind this lock; a concurrent shutdown can't invalidate session_ mid-call.
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        if (!session_) {
            CFRelease(sampleBuffer);
            return false;
        }
        status = VTDecompressionSessionDecodeFrame(
            session_,
            sampleBuffer,
            flags,
            ptsContext,  // sourceFrameRefCon
            &infoFlagsOut);
    }

    CFRelease(sampleBuffer);

    if (status != noErr) {
        ++consecutiveErrors_;
        lastError_.store(static_cast<int>(status), std::memory_order_relaxed);
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
            resetBackoffUs_ = std::min(resetBackoffUs_ * 2, media::video_config::decoder_reset::kMaxBackoffUs);
            if (keyFrameRequestFn_) keyFrameRequestFn_();
        }
        return false;
    }

    consecutiveErrors_ = 0;
    resetBackoffUs_ = media::video_config::decoder_reset::kInitialBackoffUs;
    return true;

    }  // @autoreleasepool
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
        self->lastError_.store(static_cast<int>(status), std::memory_order_relaxed);
        // Async VT errors are not paired with a submitFrame return code, so the
        // submitFrame error ladder never fires. Request a keyframe so the next
        // submit re-creates the session and re-syncs. keyFrameRequestFn_ is set
        // once at init and only read (never rewritten), so concurrent read from
        // this VT-internal thread vs decode-thread reads is safe.
        if (self->keyFrameRequestFn_) self->keyFrameRequestFn_();
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
    void* raw = layerRaw_.load(std::memory_order_acquire);
    if (!imageBuffer || !raw) return;
    AVSampleBufferDisplayLayer* layer = (__bridge AVSampleBufferDisplayLayer*)raw;

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
    // Capture strong ref to layer to prevent use-after-free if VP9Decoder is
    // destroyed while the block is queued. ARC retains the captured local.
    AVSampleBufferDisplayLayer* capturedLayer = layer;
    CFRetain(sampleBuffer);
    dispatch_async(renderQueue_, ^{
        if ([capturedLayer isReadyForMoreMediaData]) {
            [capturedLayer enqueueSampleBuffer:sampleBuffer];
        }
        CFRelease(sampleBuffer);
    });
    CFRelease(sampleBuffer);
}

}  // namespace videomodule

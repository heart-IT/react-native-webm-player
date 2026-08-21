#import "WebmVideoDecoder.h"

#import <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <deque>
#include <mutex>
#include "video/VP9HeaderParser.h"
#include "common/MediaLog.h"

namespace {

// vpcC (VP Codec Configuration Record) — 12 bytes, ISO/IEC 14496-15 Annex H.
// Required as a SampleDescriptionExtensionAtoms entry on iOS 14+; mandatory
// since the iOS 26.2 supplemental-decoder reclassification.
// Advisory only — presentation order and pacing come from each sample's PTS;
// this fills the CMSampleTimingInfo duration field for a nominal 30 fps stream.
constexpr CMTime kNominalFrameDuration = {1, 30, kCMTimeFlags_Valid, 0};

NSData* BuildVpcCConfig(int profile) {
    uint8_t buf[12] = {0};
    buf[0] = 0x01;
    buf[4] = static_cast<uint8_t>(profile);
    buf[5] = 30;                            // level 3.0 (covers 720p30)
    buf[6] = (8u << 4) | (1u << 1) | 0u;    // 8-bit, 4:2:0, limited range
    buf[7] = 1; buf[8] = 1; buf[9] = 1;     // BT.709
    return [NSData dataWithBytes:buf length:sizeof(buf)];
}

}  // namespace

@implementation WebmVideoDecoder {
    __weak AVSampleBufferDisplayLayer* _layer;
    VTDecompressionSessionRef _session;
    CMVideoFormatDescriptionRef _formatDesc;
    int _width;
    int _height;
    int _profile;
    int _containerWidth;
    int _containerHeight;
    BOOL _hwSupported;
    dispatch_queue_t _renderQueue;
    // Decoded frames waiting for the layer to pull them. Enqueuing directly from
    // the VideoToolbox callback dropped every frame the layer was not
    // instantaneously ready for, which is most of them.
    std::deque<CMSampleBufferRef> _pending;
    std::mutex _pendingMutex;
    // submitFrame runs on the demux thread; suspend/resume arrive on main.
    // Guards _session and _formatDesc against teardown mid-submit. Taken only at
    // public entry points — ensureSession/shutdownSession assume it is held.
    std::mutex _sessionMutex;
    BOOL _suspended;
    BOOL _requesting;  // _renderQueue only
    std::atomic<uint64_t> _framesPresented;
    std::atomic<uint64_t> _pendingCapDrops;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _renderQueue = dispatch_queue_create("webmplayer.video.render", DISPATCH_QUEUE_SERIAL);
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    if (@available(iOS 26.2, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(kVP9CodecType);
    }
    _hwSupported = VTIsHardwareDecodeSupported(kVP9CodecType);
    if (!_hwSupported) {
        MEDIA_LOG_E("WebmVideoDecoder: no VP9 decoder on this device; video cannot play");
    }
    return self;
}

- (void)dealloc { [self shutdown]; }

- (BOOL)hardwareDecodeSupported { return _hwSupported; }

- (uint64_t)framesPresented { return _framesPresented.load(); }

- (void)armRequest {
    AVSampleBufferDisplayLayer* layer = _layer;
    if (!layer || _requesting) return;
    _requesting = YES;
    __weak WebmVideoDecoder* weakSelf = self;
    [layer requestMediaDataWhenReadyOnQueue:_renderQueue usingBlock:^{
        [weakSelf feedLayer:layer];
    }];
}

- (void)feedLayer:(AVSampleBufferDisplayLayer*)layer {
    if (layer.status == AVQueuedSampleBufferRenderingStatusFailed) {
        MEDIA_LOG_E("WebmVideoDecoder: display layer failed: %s",
                    layer.error.localizedDescription.UTF8String);
        [layer flush];
    }
    while (layer.isReadyForMoreMediaData) {
        CMSampleBufferRef next = nullptr;
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            if (_pending.empty()) {
                // Same contract as the audio renderer: returning while the layer
                // is still ready ends the requests, so cancel and re-arm when the
                // next frame is decoded.
                _requesting = NO;
                [layer stopRequestingMediaData];
                return;
            }
            next = _pending.front();
            _pending.pop_front();
        }
        [layer enqueueSampleBuffer:next];
        CFRelease(next);
        _framesPresented.fetch_add(1);
    }
}

- (void)drainPending {
    std::lock_guard<std::mutex> lock(_pendingMutex);
    for (CMSampleBufferRef sb : _pending) CFRelease(sb);
    _pending.clear();
}

- (void)setOutputLayer:(AVSampleBufferDisplayLayer*)layer { _layer = layer; }

- (void)setContainerWidth:(int)width height:(int)height {
    _containerWidth = width;
    _containerHeight = height;
}

- (BOOL)ensureSession:(int)width height:(int)height profile:(int)profile {
    if (_session && width == _width && height == _height && profile == _profile) return YES;
    [self shutdownSession];
    if (!_hwSupported) return NO;  // already logged once at init

    NSData* vpcc = BuildVpcCConfig(profile);
    NSDictionary* extensions = @{
        (NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: @{ @"vpcC": vpcc }
    };
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    OSStatus s = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kVP9CodecType,
                                                width, height,
                                                (__bridge CFDictionaryRef)extensions, &_formatDesc);
    if (s != noErr || !_formatDesc) {
        MEDIA_LOG_E("WebmVideoDecoder: CMVideoFormatDescriptionCreate failed: %d", (int)s);
        return NO;
    }

    NSDictionary* destAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    VTDecompressionOutputCallbackRecord cb = {0};
    cb.decompressionOutputCallback = &OutputCallback;
    cb.decompressionOutputRefCon = (__bridge void*)self;
    s = VTDecompressionSessionCreate(kCFAllocatorDefault, _formatDesc, nullptr,
                                     (__bridge CFDictionaryRef)destAttrs, &cb, &_session);
    if (s != noErr || !_session) {
        MEDIA_LOG_E("WebmVideoDecoder: VTDecompressionSessionCreate failed: %d", (int)s);
        CFRelease(_formatDesc); _formatDesc = nullptr;
        return NO;
    }
    _width = width; _height = height; _profile = profile;
    // Once per session, and the only evidence that a post-background rebuild
    // actually happened — the frozen-picture diagnosis hangs on this line.
    MEDIA_LOG_I("WebmVideoDecoder: session created %dx%d profile %d", width, height, profile);
    return YES;
}

- (BOOL)submitFrame:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs isKey:(BOOL)isKey {
    if (!data || length == 0) return NO;

    std::lock_guard<std::mutex> lock(_sessionMutex);
    // Backgrounded: decoder resources are not ours to use, and rebuilding the
    // session here would fail. Frames resume at the next keyframe after resume.
    if (_suspended) return NO;

    if (isKey) {
        auto info = media::vp9::parseHeader(data, length);
        if (info.valid && info.width > 0 && info.height > 0) {
            if (![self ensureSession:info.width height:info.height profile:info.profile]) return NO;
        } else if (!_session && _containerWidth > 0 && _containerHeight > 0) {
            // Header unreadable. The container already declared the frame size,
            // so fall back to it rather than never creating a session. Profile 0
            // is the only one WebM/VP9 mandates support for, and a wrong guess
            // surfaces as a decode error rather than silence.
            MEDIA_LOG_W("WebmVideoDecoder: VP9 header unparseable, using container %dx%d",
                        _containerWidth, _containerHeight);
            if (![self ensureSession:_containerWidth height:_containerHeight profile:0]) return NO;
        }
    }
    if (!_session) return NO;

    CMBlockBufferRef block = nullptr;
    OSStatus s = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, length,
                                                    kCFAllocatorDefault, nullptr, 0, length,
                                                    kCMBlockBufferAssureMemoryNowFlag, &block);
    if (s != noErr || !block) return NO;
    s = CMBlockBufferReplaceDataBytes(data, block, 0, length);
    if (s != noErr) {
        // Submitting the block anyway would decode uninitialized bytes.
        MEDIA_LOG_E("WebmVideoDecoder: block buffer copy failed: %d", (int)s);
        CFRelease(block);
        return NO;
    }

    size_t sampleSize = length;
    CMSampleTimingInfo timing;
    timing.duration = kNominalFrameDuration;
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample = nullptr;
    s = CMSampleBufferCreateReady(kCFAllocatorDefault, block, _formatDesc, 1, 1, &timing,
                                  1, &sampleSize, &sample);
    CFRelease(block);
    if (s != noErr || !sample) return NO;

    void* ptsCtx = reinterpret_cast<void*>(static_cast<uintptr_t>(ptsUs));
    VTDecodeInfoFlags out = 0;
    s = VTDecompressionSessionDecodeFrame(_session, sample,
                                          kVTDecodeFrame_EnableAsynchronousDecompression,
                                          ptsCtx, &out);
    CFRelease(sample);

    // iOS revokes video decoder resources when the app loses the foreground, and
    // the session stays invalid afterwards. Nothing else reports this, so without
    // tearing it down here a session that outlived a background trip would be
    // reused forever and never decode another frame.
    if (s == kVTInvalidSessionErr) {
        MEDIA_LOG_W("WebmVideoDecoder: decode session invalidated, rebuilding at next keyframe");
        [self shutdownSession];
        return NO;
    }
    if (s != noErr) {
        MEDIA_LOG_E("WebmVideoDecoder: decode failed: %d", (int)s);
        return NO;
    }
    return YES;
}

static void OutputCallback(void* refCon, void* sourceFrameRefCon, OSStatus status,
                            VTDecodeInfoFlags flags, CVImageBufferRef imageBuffer,
                            CMTime, CMTime) {
    if (status != noErr) {
        MEDIA_LOG_E("WebmVideoDecoder: decode callback error %d", (int)status);
        return;
    }
    if (!imageBuffer || (flags & kVTDecodeInfo_FrameDropped)) return;
    auto* self_ = (__bridge WebmVideoDecoder*)refCon;
    int64_t ptsUs = static_cast<int64_t>(reinterpret_cast<uintptr_t>(sourceFrameRefCon));

    CMVideoFormatDescriptionRef outFmt = nullptr;
    if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, imageBuffer, &outFmt) != noErr) return;

    CMSampleTimingInfo timing;
    timing.duration = kNominalFrameDuration;
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample = nullptr;
    OSStatus s = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, imageBuffer, outFmt, &timing, &sample);
    CFRelease(outFmt);
    if (s != noErr || !sample) return;

    {
        std::lock_guard<std::mutex> lock(self_->_pendingMutex);
        // Bound the backlog at ~1s of 30fps video; a layer that stops pulling
        // must not grow this without limit.
        if (self_->_pending.size() >= 30) {
            CFRelease(sample);
            // A pinned-full queue with every new frame discarded is exactly the
            // frozen-layer signature, and it was previously invisible: counted
            // nowhere, logged never. Rate-limited so a wedged layer logs about
            // four lines a minute, not thirty a second.
            uint64_t drops = self_->_pendingCapDrops.fetch_add(1) + 1;
            if ((drops & 511) == 1) {
                MEDIA_LOG_W("WebmVideoDecoder: pending queue full, %llu frames dropped",
                            static_cast<unsigned long long>(drops));
            }
            return;
        }
        self_->_pending.push_back(sample);  // ownership moves to the queue
    }
    dispatch_async(self_->_renderQueue, ^{
        [self_ armRequest];
    });
}

- (void)shutdownSession {
    if (_session) {
        // Drain in-flight frames before invalidating; the OutputCallback captures `self`
        // via __bridge (non-owning). Without this wait, a callback fired post-invalidation
        // would dereference a half-destroyed Objective-C object.
        VTDecompressionSessionWaitForAsynchronousFrames(_session);
        VTDecompressionSessionInvalidate(_session);
        CFRelease(_session); _session = nullptr;
    }
    if (_formatDesc) { CFRelease(_formatDesc); _formatDesc = nullptr; }
    _width = _height = _profile = 0;
}

- (void)suspend {
    std::lock_guard<std::mutex> lock(_sessionMutex);
    if (_suspended) return;
    _suspended = YES;
    // Released rather than left to be revoked, so the decoder is handed back
    // cleanly. Already-decoded frames stay queued: the synchronizer's clock is
    // stopped too, so their timestamps are still ahead of it on resume and they
    // paint the first picture back with no wait for a keyframe.
    [self shutdownSession];
}

- (void)resume {
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        _suspended = NO;
    }

    AVSampleBufferDisplayLayer* layer = _layer;
    if (!layer) return;
    // Losing the foreground leaves the layer in AVQueuedSampleBufferRenderingStatusFailed,
    // and it only leaves that state on a -flush. The flush in feedLayer cannot do it: a
    // failed layer never becomes ready for more media data, so the request block it runs
    // from is never invoked again. Waiting to be asked means never being asked, and the
    // picture stays frozen even though the decoder recovered at the next keyframe.
    dispatch_async(_renderQueue, ^{
        long statusBefore = (long)layer.status;
        BOOL needsFlush = layer.status == AVQueuedSampleBufferRenderingStatusFailed ||
                          layer.requiresFlushToResumeDecoding;
        if (needsFlush) {
            [layer flush];
        }
        // Pending PTS are timescale-1e6 by construction (OutputCallback), so
        // .value reads back as microseconds directly.
        size_t pendingCount = 0;
        int64_t frontPtsUs = -1;
        int64_t backPtsUs = -1;
        {
            std::lock_guard<std::mutex> lock(self->_pendingMutex);
            pendingCount = self->_pending.size();
            if (pendingCount > 0) {
                frontPtsUs = CMSampleBufferGetPresentationTimeStamp(self->_pending.front()).value;
                backPtsUs = CMSampleBufferGetPresentationTimeStamp(self->_pending.back()).value;
            }
        }
        // Discriminates the open frozen-picture candidates: a layer still Failed
        // after the flush needs re-adding to the synchronizer, not flushing; a
        // pending queue whose PTS trail the resumed clock is discarded as late.
        MEDIA_LOG_I("WebmVideoDecoder: resume: status %ld->%ld flushed %d, %zu pending pts %lld..%lld us",
                    statusBefore, (long)layer.status, (int)needsFlush,
                    pendingCount, (long long)frontPtsUs, (long long)backPtsUs);
        // Re-arm from scratch: the request this decoder installed before the trip is
        // dead along with the layer's old status.
        if (self->_requesting) {
            self->_requesting = NO;
            [layer stopRequestingMediaData];
        }
        [self armRequest];
    });
}

- (void)shutdown {
    std::lock_guard<std::mutex> lock(_sessionMutex);
    [self shutdownSession];
    if (_requesting) {
        [_layer stopRequestingMediaData];
        _requesting = NO;
    }
    [self drainPending];
    // Flush the render queue to ensure no enqueued blocks reference stale buffers.
    dispatch_sync(_renderQueue, ^{});
}

@end

#import "WebmVideoDecoder.h"

#import "WebmDisplayLayerWatch.h"
#import "WebmVP9Session.h"

#import <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <deque>
#include <mutex>
#include "video/VP9HeaderParser.h"
#include "common/MediaLog.h"

namespace {

// Advisory only — presentation order and pacing come from each sample's PTS;
// this fills the CMSampleTimingInfo duration field for a nominal 30 fps stream.
constexpr CMTime kNominalFrameDuration = {1, 30, kCMTimeFlags_Valid, 0};

// Bounds the decoded-frame backlog at ~1s of 30fps video; a layer that stops
// pulling must not grow it without limit.
constexpr size_t kMaxPendingFrames = 30;

// Drop logs sample every 512th occurrence: a wedged layer drops ~30/s.
constexpr uint64_t kDropLogMask = 511;

// Presented-frame progress: one line per ~10 s at 30 fps.
constexpr uint64_t kPresentedLogEvery = 300;

// Marks _renderQueue so teardown can tell it is already on it: a dispatch_sync
// onto itself deadlocks, and dealloc can run from a queued block's last ref.
void* const kRenderQueueKey = (void*)&kRenderQueueKey;

}  // namespace

@implementation WebmVideoDecoder {
    __weak AVSampleBufferDisplayLayer* _layer;
    VTDecompressionSessionRef _session;
    CMVideoFormatDescriptionRef _formatDesc;
    int _width;
    int _height;
    int _profile;
    BOOL _hwSupported;
    dispatch_queue_t _renderQueue;
    // Decoded frames waiting for the layer to pull them: enqueuing straight from
    // the VideoToolbox callback dropped every frame the layer was not ready for.
    std::deque<CMSampleBufferRef> _pending;
    std::mutex _pendingMutex;
    // submitFrame runs on the demux thread; shutdown and reset arrive on the
    // engine's control thread. Guards _session and _formatDesc against teardown
    // mid-submit. Taken only at public entry points — ensureSession and
    // shutdownSession assume it is held.
    std::mutex _sessionMutex;
    WebmDisplayLayerWatch* _layerWatch;  // main thread only
    BOOL _requesting;  // _renderQueue only
    std::atomic<uint64_t> _framesPresented;
    std::atomic<uint64_t> _pendingCapDrops;
    std::atomic<int64_t> _minDecodeLeadUs;
    std::atomic<int64_t> _minEnqueueLeadUs;
    std::atomic<uint64_t> _lateEnqueues;
}

static void NoteMin(std::atomic<int64_t>& slot, int64_t value) {
    int64_t cur = slot.load();
    while (value < cur && !slot.compare_exchange_weak(cur, value)) {}
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _renderQueue = dispatch_queue_create("webmplayer.video.render", DISPATCH_QUEUE_SERIAL);
    _minDecodeLeadUs = INT64_MAX;
    _minEnqueueLeadUs = INT64_MAX;
    dispatch_queue_set_specific(_renderQueue, kRenderQueueKey, (void*)1, nullptr);
    if (@available(iOS 26.2, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(kWebmVP9CodecType);
    }
    _hwSupported = VTIsHardwareDecodeSupported(kWebmVP9CodecType);
    if (!_hwSupported) {
        MEDIA_LOG_E("WebmVideoDecoder: no VP9 decoder on this device; video cannot play");
    }
    return self;
}

- (void)dealloc { [self shutdown]; }

- (BOOL)hardwareDecodeSupported { return _hwSupported; }

- (BOOL)hasSession {
    std::lock_guard<std::mutex> lock(_sessionMutex);
    return _session != nullptr;
}

- (uint64_t)framesPresented { return _framesPresented.load(); }

- (uint64_t)pendingCapDrops { return _pendingCapDrops.load(); }

- (size_t)pendingFrames {
    std::lock_guard<std::mutex> lock(_pendingMutex);
    return _pending.size();
}

- (int64_t)leadUs:(int64_t)ptsUs {
    double (^clock)(void) = _clock;
    double now = clock ? clock() : -1.0;
    return now < 0 ? INT64_MAX : ptsUs - static_cast<int64_t>(now * 1000000.0);
}

- (NSString*)leadReport {
    int64_t decode = _minDecodeLeadUs.exchange(INT64_MAX);
    int64_t enqueue = _minEnqueueLeadUs.exchange(INT64_MAX);
    return [NSString stringWithFormat:@"lead decode %.2fs enqueue %.2fs late %llu",
                                      decode == INT64_MAX ? NAN : decode / 1000000.0,
                                      enqueue == INT64_MAX ? NAN : enqueue / 1000000.0,
                                      static_cast<unsigned long long>(_lateEnqueues.load())];
}

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
        int64_t lead = [self leadUs:static_cast<int64_t>(
                                        CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(next)) * 1000000.0)];
        NoteMin(_minEnqueueLeadUs, lead);
        if (lead < 0) _lateEnqueues.fetch_add(1);
        [layer enqueueSampleBuffer:next];
        CFRelease(next);
        // In a device log, these lines are the evidence video reaches the layer.
        uint64_t presented = _framesPresented.fetch_add(1) + 1;
        if (presented == 1 || presented % kPresentedLogEvery == 0) {
            MEDIA_LOG_I("WebmVideoDecoder: %llu frames presented",
                        static_cast<unsigned long long>(presented));
        }
    }
}

- (void)drainPending {
    std::lock_guard<std::mutex> lock(_pendingMutex);
    for (CMSampleBufferRef sb : _pending) CFRelease(sb);
    _pending.clear();
}

- (void)setOutputLayer:(AVSampleBufferDisplayLayer*)layer {
    [_layerWatch invalidate];
    _layerWatch = nil;
    _layer = layer;
    if (!layer) return;
    __weak WebmVideoDecoder* weakSelf = self;
    _layerWatch = [[WebmDisplayLayerWatch alloc] initWithLayer:layer
                                                 onFlushNeeded:^{
                                                     [weakSelf recoverLayer:layer];
                                                 }];
}

- (void)logLayerPerformance {
    [_layerWatch logPerformance];
}

// The layer reported that only a flush will make it accept media again.
- (void)recoverLayer:(AVSampleBufferDisplayLayer*)layer {
    dispatch_async(_renderQueue, ^{
        long before = (long)layer.status;
        [layer flush];
        // Decoded while the layer was failed, so already behind the clock.
        [self drainPending];
        MEDIA_LOG_I("WebmVideoDecoder: layer required flush: status %ld->%ld",
                    before, (long)layer.status);
        // Re-arm from scratch: the request installed before the failure died
        // with the layer's old status.
        if (self->_requesting) {
            self->_requesting = NO;
            [layer stopRequestingMediaData];
        }
        [self armRequest];
    });
}

- (BOOL)ensureSession:(int)width height:(int)height profile:(int)profile {
    if (_session && width == _width && height == _height && profile == _profile) return YES;
    [self shutdownSession];
    if (!_hwSupported) return NO;  // already logged once at init

    _formatDesc = WebmCreateVP9FormatDescription(width, height, profile);
    if (!_formatDesc) return NO;
    _session = WebmCreateVP9Session(_formatDesc, width, height, &OutputCallback, (__bridge void*)self);
    if (!_session) {
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

    if (isKey) {
        auto info = media::vp9::parseHeader(data, length);
        if (info.valid && info.width > 0 && info.height > 0 &&
            ![self ensureSession:info.width height:info.height profile:info.profile]) {
            return NO;
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
    NoteMin(self_->_minDecodeLeadUs, [self_ leadUs:ptsUs]);

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
        if (self_->_pending.size() >= kMaxPendingFrames) {
            CFRelease(sample);
            // A pinned-full queue with every new frame discarded is exactly the
            // frozen-layer signature, and it was previously invisible: counted
            // nowhere, logged never. Rate-limited so a wedged layer logs about
            // four lines a minute, not thirty a second.
            uint64_t drops = self_->_pendingCapDrops.fetch_add(1) + 1;
            if ((drops & kDropLogMask) == 1) {
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

// Discards everything belonging to the previous stream: the session (in-flight
// callbacks fenced by shutdownSession; the next keyframe rebuilds), the decoded
// backlog, and the frames already enqueued on the layer. Called on stream
// reset — the old timeline's frames would present as late against the
// re-anchored clock.
- (void)resetForNewStream {
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        [self shutdownSession];
    }
    [self drainPending];
    AVSampleBufferDisplayLayer* layer = _layer;
    if (!layer) return;
    dispatch_async(_renderQueue, ^{
        [layer flush];
    });
}

- (void)shutdown {
    [_layerWatch invalidate];
    _layerWatch = nil;
    std::lock_guard<std::mutex> lock(_sessionMutex);
    [self shutdownSession];
    if (_requesting) {
        [_layer stopRequestingMediaData];
        _requesting = NO;
    }
    [self drainPending];
    // Flush the render queue so no enqueued block still references stale
    // buffers — unless this IS the render queue (dealloc ran from a queued
    // block's last strong reference), where the sync would deadlock and is
    // also unnecessary: a serial queue runs nothing else while this executes,
    // and no queued block can reference self after it, or dealloc could not
    // be running.
    if (!dispatch_get_specific(kRenderQueueKey)) {
        dispatch_sync(_renderQueue, ^{});
    }
}

@end

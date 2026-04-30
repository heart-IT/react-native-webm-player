#import "PlaybackEngineV2VideoDecoder.h"

#import <VideoToolbox/VideoToolbox.h>
#include "video/VP9HeaderParser.h"
#include "common/MediaLog.h"

namespace {

// vpcC (VP Codec Configuration Record) — 12 bytes, ISO/IEC 14496-15 Annex H.
// Required as a SampleDescriptionExtensionAtoms entry on iOS 14+; mandatory
// since the iOS 26.2 supplemental-decoder reclassification.
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

@implementation PlaybackEngineV2VideoDecoder {
    __weak AVSampleBufferDisplayLayer* _layer;
    VTDecompressionSessionRef _session;
    CMVideoFormatDescriptionRef _formatDesc;
    int _width;
    int _height;
    int _profile;
    BOOL _hwSupported;
    dispatch_queue_t _renderQueue;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _renderQueue = dispatch_queue_create("playbackv2.video.render", DISPATCH_QUEUE_SERIAL);
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    if (@available(iOS 26.2, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(kVP9CodecType);
    }
    _hwSupported = VTIsHardwareDecodeSupported(kVP9CodecType);
    return self;
}

- (void)dealloc { [self shutdown]; }

- (void)setOutputLayer:(AVSampleBufferDisplayLayer*)layer { _layer = layer; }

- (BOOL)ensureSession:(int)width height:(int)height profile:(int)profile {
    if (_session && width == _width && height == _height && profile == _profile) return YES;
    [self shutdownSession];
    if (!_hwSupported) return NO;

    NSData* vpcc = BuildVpcCConfig(profile);
    NSDictionary* extensions = @{
        (NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: @{ @"vpcC": vpcc }
    };
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    OSStatus s = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kVP9CodecType,
                                                width, height,
                                                (__bridge CFDictionaryRef)extensions, &_formatDesc);
    if (s != noErr || !_formatDesc) {
        MEDIA_LOG_E("PlaybackEngineV2VideoDecoder: CMVideoFormatDescriptionCreate failed: %d", (int)s);
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
        MEDIA_LOG_E("PlaybackEngineV2VideoDecoder: VTDecompressionSessionCreate failed: %d", (int)s);
        CFRelease(_formatDesc); _formatDesc = nullptr;
        return NO;
    }
    _width = width; _height = height; _profile = profile;
    return YES;
}

- (BOOL)submitFrame:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs isKey:(BOOL)isKey {
    if (!data || length == 0) return NO;

    if (isKey) {
        auto info = media::vp9::parseHeader(data, length);
        if (info.valid && info.width > 0 && info.height > 0) {
            if (![self ensureSession:info.width height:info.height profile:info.profile]) return NO;
        }
    }
    if (!_session) return NO;

    CMBlockBufferRef block = nullptr;
    OSStatus s = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, length,
                                                    kCFAllocatorDefault, nullptr, 0, length,
                                                    kCMBlockBufferAssureMemoryNowFlag, &block);
    if (s != noErr || !block) return NO;
    CMBlockBufferReplaceDataBytes(data, block, 0, length);

    size_t sampleSize = length;
    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, 30);
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
    return s == noErr;
}

static void OutputCallback(void* refCon, void* sourceFrameRefCon, OSStatus status,
                            VTDecodeInfoFlags flags, CVImageBufferRef imageBuffer,
                            CMTime, CMTime) {
    if (status != noErr || !imageBuffer || (flags & kVTDecodeInfo_FrameDropped)) return;
    auto* self_ = (__bridge PlaybackEngineV2VideoDecoder*)refCon;
    int64_t ptsUs = static_cast<int64_t>(reinterpret_cast<uintptr_t>(sourceFrameRefCon));

    CMVideoFormatDescriptionRef outFmt = nullptr;
    if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, imageBuffer, &outFmt) != noErr) return;

    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, 30);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample = nullptr;
    OSStatus s = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, imageBuffer, outFmt, &timing, &sample);
    CFRelease(outFmt);
    if (s != noErr || !sample) return;

    AVSampleBufferDisplayLayer* layer = self_->_layer;
    CFRetain(sample);
    dispatch_async(self_->_renderQueue, ^{
        if (layer && layer.isReadyForMoreMediaData) [layer enqueueSampleBuffer:sample];
        CFRelease(sample);
    });
    CFRelease(sample);
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

- (void)shutdown {
    [self shutdownSession];
    // Flush the render queue to ensure no enqueued blocks reference stale buffers.
    dispatch_sync(_renderQueue, ^{});
}

@end

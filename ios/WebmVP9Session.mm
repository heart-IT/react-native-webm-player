#import "WebmVP9Session.h"

#include "common/MediaLog.h"

const CMVideoCodecType kWebmVP9CodecType = 'vp09';

namespace {

// vpcC (VP Codec Configuration Record), 12 bytes, ISO/IEC 14496-15 Annex H:
// a SampleDescriptionExtensionAtoms entry VideoToolbox requires for VP9.
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

CMVideoFormatDescriptionRef WebmCreateVP9FormatDescription(int width, int height, int profile) {
    NSDictionary* extensions = @{
        (NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms:
            @{ @"vpcC": BuildVpcCConfig(profile) }
    };
    CMVideoFormatDescriptionRef format = nullptr;
    OSStatus s = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kWebmVP9CodecType,
                                                width, height,
                                                (__bridge CFDictionaryRef)extensions, &format);
    if (s != noErr || !format) {
        MEDIA_LOG_E("WebmVideoDecoder: CMVideoFormatDescriptionCreate failed: %d", (int)s);
        return nullptr;
    }
    return format;
}

VTDecompressionSessionRef WebmCreateVP9Session(CMVideoFormatDescriptionRef format,
                                               int width, int height,
                                               VTDecompressionOutputCallback callback,
                                               void* refCon) {
    NSDictionary* destAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    VTDecompressionOutputCallbackRecord cb = {0};
    cb.decompressionOutputCallback = callback;
    cb.decompressionOutputRefCon = refCon;
    VTDecompressionSessionRef session = nullptr;
    OSStatus s = VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr,
                                              (__bridge CFDictionaryRef)destAttrs, &cb, &session);
    if (s != noErr || !session) {
        MEDIA_LOG_E("WebmVideoDecoder: VTDecompressionSessionCreate failed: %d", (int)s);
        return nullptr;
    }
    return session;
}

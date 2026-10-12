// The VideoToolbox pieces a VP9 stream needs: the format description carrying
// the vpcC atom, and the hardware decompression session that turns packets
// into IOSurface-backed NV12 pixel buffers. Construction only; WebmVideoDecoder
// owns and drives what these return.
#pragma once

#import <VideoToolbox/VideoToolbox.h>

NS_ASSUME_NONNULL_BEGIN

/// 'vp09', the codec type VideoToolbox registers the VP9 decoder under.
extern const CMVideoCodecType kWebmVP9CodecType;

/// Returns a format description the caller owns, or null after logging why.
CMVideoFormatDescriptionRef _Nullable WebmCreateVP9FormatDescription(int width, int height,
                                                                     int profile);

/// Returns a session the caller owns, decoding into width x height buffers and
/// delivering through `callback` with `refCon`, or null after logging why.
VTDecompressionSessionRef _Nullable WebmCreateVP9Session(CMVideoFormatDescriptionRef format,
                                                         int width, int height,
                                                         VTDecompressionOutputCallback callback,
                                                         void* _Nullable refCon);

NS_ASSUME_NONNULL_END

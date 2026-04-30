// React Native view component for v2 video output.
// Wraps an AVSampleBufferDisplayLayer; on attach, registers itself as the
// current PlaybackEngineV2's display layer. On detach, clears the registration.
#pragma once

#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface VideoViewV2 : UIView

@property (nonatomic, readonly) AVSampleBufferDisplayLayer* displayLayer;

/// Set the layer's videoGravity. Maps to:
///   0 = AVLayerVideoGravityResize         (stretch fill)
///   1 = AVLayerVideoGravityResizeAspect   (fit, default)
///   2 = AVLayerVideoGravityResizeAspectFill (cover)
- (void)setScaleMode:(NSInteger)mode;

@end

NS_ASSUME_NONNULL_END

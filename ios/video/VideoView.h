// Native UIView subclass for rendering decoded VP9 frames via AVSampleBufferDisplayLayer.
// Managed by VideoViewManager for React Native component registration.
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

@interface VideoView : UIView

@property (nonatomic, assign) BOOL mirror;
@property (nonatomic, assign) NSInteger scaleMode;

@end

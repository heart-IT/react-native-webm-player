#import "VideoViewV2.h"
#import "PlaybackEngineV2.h"
#import "MediaPipelineModuleV2.h"

#import <React/RCTViewManager.h>

@implementation VideoViewV2

+ (Class)layerClass { return [AVSampleBufferDisplayLayer class]; }

- (AVSampleBufferDisplayLayer*)displayLayer {
    return (AVSampleBufferDisplayLayer*)self.layer;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.backgroundColor = [UIColor blackColor];
    self.displayLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    return self;
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    PlaybackEngineV2* engine = mediamodule_v2::currentEngine();
    if (!engine) return;
    if (self.window) {
        // Attach: this view becomes the engine's render target. Unattaching the previous
        // VideoViewV2 (if any) is the engine's responsibility — see setDisplayLayer:.
        [engine setDisplayLayer:self.displayLayer];
    } else {
        // Window torn down. Clear the engine's reference if it still points at us, so the
        // next attached view takes over cleanly.
        [engine setDisplayLayer:nil];
    }
}

- (void)setScaleMode:(NSInteger)mode {
    NSString* gravity = AVLayerVideoGravityResizeAspect;
    switch (mode) {
        case 0: gravity = AVLayerVideoGravityResize;            break;
        case 1: gravity = AVLayerVideoGravityResizeAspect;      break;
        case 2: gravity = AVLayerVideoGravityResizeAspectFill;  break;
        default: break;
    }
    self.displayLayer.videoGravity = gravity;
}

@end

#pragma mark - View manager

@interface VideoViewV2Manager : RCTViewManager
@end

@implementation VideoViewV2Manager

RCT_EXPORT_MODULE(WebmPlayerV2View)

- (UIView*)view {
    return [[VideoViewV2 alloc] init];
}

RCT_CUSTOM_VIEW_PROPERTY(scaleMode, NSInteger, VideoViewV2) {
    NSInteger mode = json ? [(NSNumber*)json integerValue] : 1;
    [view setScaleMode:mode];
}

@end

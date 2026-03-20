#import "VideoView.h"
#include "../../cpp/video/VideoSurfaceRegistry.h"
#include "../../cpp/common/MediaLog.h"

@implementation VideoView {
    AVSampleBufferDisplayLayer* _displayLayer;
    BOOL _registered;
}

+ (Class)layerClass {
    return [AVSampleBufferDisplayLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _mirror = NO;
        _scaleMode = 1;
        _registered = NO;
        _displayLayer = (AVSampleBufferDisplayLayer *)self.layer;
        _displayLayer.videoGravity = AVLayerVideoGravityResizeAspectFill;
    }
    return self;
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window && !_registered) {
        void* surface = (__bridge void*)_displayLayer;
        CFRetain(surface);
        media::VideoSurfaceRegistry::instance().registerSurface(surface);
        _registered = YES;
    } else if (!self.window && _registered) {
        media::VideoSurfaceRegistry::instance().unregisterSurface();
        _registered = NO;
    }
}

- (void)dealloc {
    if (_registered) {
        media::VideoSurfaceRegistry::instance().unregisterSurface();
    }
}

- (void)setMirror:(BOOL)mirror {
    _mirror = mirror;
    self.layer.transform = mirror ? CATransform3DMakeScale(-1.0, 1.0, 1.0) : CATransform3DIdentity;
}

- (void)setScaleMode:(NSInteger)scaleMode {
    _scaleMode = scaleMode;
    _displayLayer.videoGravity = (scaleMode == 0)
        ? AVLayerVideoGravityResizeAspect
        : AVLayerVideoGravityResizeAspectFill;
}

@end

#import "VideoView.h"
#include "../../cpp/video/VideoSurfaceRegistry.h"
#include "../../cpp/common/MediaLog.h"

@implementation VideoView {
    AVSampleBufferDisplayLayer* _displayLayer;
}

+ (Class)layerClass {
    return [AVSampleBufferDisplayLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _participantId = -1;
        _mirror = NO;
        _scaleMode = 1;
        _displayLayer = (AVSampleBufferDisplayLayer *)self.layer;
        _displayLayer.videoGravity = AVLayerVideoGravityResizeAspectFill;
    }
    return self;
}

- (void)dealloc {
    if (_participantId >= 0) {
        media::VideoSurfaceRegistry::instance().unregisterSurface(static_cast<int>(_participantId));
    }
}

- (void)setParticipantId:(NSInteger)participantId {
    if (_participantId == participantId) return;

    if (_participantId >= 0) {
        media::VideoSurfaceRegistry::instance().unregisterSurface(static_cast<int>(_participantId));
    }

    _participantId = participantId;

    if (_participantId >= 0) {
        // Register the display layer as the surface for this participant.
        // The registry's acquireFn will retain the layer; we pass a +0 ref
        // and the registry takes ownership via acquire.
        void* surface = (__bridge void*)_displayLayer;
        CFRetain(surface);
        media::VideoSurfaceRegistry::instance().registerSurface(
            static_cast<int>(_participantId), surface);
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

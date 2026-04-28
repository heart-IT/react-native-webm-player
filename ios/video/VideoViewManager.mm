#import "VideoViewManager.h"
#import "VideoView.h"

@implementation VideoViewManager

RCT_EXPORT_MODULE(WebmVideoView)

- (UIView *)view {
    return [[VideoView alloc] initWithFrame:CGRectZero];
}

RCT_EXPORT_VIEW_PROPERTY(mirror, BOOL)
RCT_EXPORT_VIEW_PROPERTY(scaleMode, NSInteger)

@end

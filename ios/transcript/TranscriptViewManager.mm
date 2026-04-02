#import "TranscriptViewManager.h"
#import "TranscriptView.h"

@implementation TranscriptViewManager

RCT_EXPORT_MODULE(WebmTranscriptView)

- (UIView *)view {
    return [[TranscriptView alloc] initWithFrame:CGRectZero];
}

RCT_EXPORT_VIEW_PROPERTY(enabled, BOOL)

@end

#import "TranscriptView.h"
#include "../../cpp/transcript/TranscriptRegistry.h"

@implementation TranscriptView {
    UILabel* _label;
    BOOL _registered;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.userInteractionEnabled = NO;
        self.clipsToBounds = YES;

        _label = [[UILabel alloc] initWithFrame:CGRectZero];
        _label.textColor = [UIColor whiteColor];
        _label.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
        _label.textAlignment = NSTextAlignmentCenter;
        _label.numberOfLines = 0;
        _label.lineBreakMode = NSLineBreakByWordWrapping;
        _label.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.6];
        _label.layer.cornerRadius = 6;
        _label.clipsToBounds = YES;
        _label.hidden = YES;
        _label.translatesAutoresizingMaskIntoConstraints = NO;

        [self addSubview:_label];
        [NSLayoutConstraint activateConstraints:@[
            [_label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:16],
            [_label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-16],
            [_label.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-16],
        ]];
    }
    return self;
}

- (void)setEnabled:(BOOL)enabled {
    _enabled = enabled;
    _label.hidden = !enabled;

    if (enabled && !_registered) {
        [self registerForTranscript];
    } else if (!enabled && _registered) {
        [self unregisterForTranscript];
    }
}

- (void)registerForTranscript {
    __weak TranscriptView* weakSelf = self;
    media::transcript::TranscriptRegistry::instance().setCallback(
        media::transcript::CallbackSlot::NativeView,
        [weakSelf](const media::transcript::TranscriptSegment& seg) {
            NSString* nsText = [NSString stringWithUTF8String:seg.text.c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{
                TranscriptView* strongSelf = weakSelf;
                if (strongSelf && strongSelf->_enabled) {
                    strongSelf->_label.text = nsText;
                    strongSelf->_label.hidden = nsText.length == 0;
                }
            });
        });
    _registered = YES;
}

- (void)unregisterForTranscript {
    media::transcript::TranscriptRegistry::instance().clearCallback(
        media::transcript::CallbackSlot::NativeView);
    _registered = NO;
    _label.text = nil;
    _label.hidden = YES;
}

- (void)dealloc {
    if (_registered) {
        media::transcript::TranscriptRegistry::instance().clearCallback(
            media::transcript::CallbackSlot::NativeView);
    }
}

@end

#import "WebmInterruptionObserver.h"

#import <AVFoundation/AVFoundation.h>

@implementation WebmInterruptionObserver {
  id _token;
}

- (instancetype)initWithOnBegan:(void (^)(void))onBegan
                        onEnded:(void (^)(BOOL))onEnded {
  self = [super init];
  if (!self) return nil;

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  _token = [center
      addObserverForName:AVAudioSessionInterruptionNotification
                  object:[AVAudioSession sharedInstance]
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* note) {
                NSNumber* typeValue =
                    note.userInfo[AVAudioSessionInterruptionTypeKey];
                auto type = (AVAudioSessionInterruptionType)
                                typeValue.unsignedIntegerValue;
                if (type == AVAudioSessionInterruptionTypeBegan) {
                  onBegan();
                } else if (type == AVAudioSessionInterruptionTypeEnded) {
                  NSNumber* opts =
                      note.userInfo[AVAudioSessionInterruptionOptionKey];
                  BOOL shouldResume = (opts.unsignedIntegerValue &
                                       AVAudioSessionInterruptionOptionShouldResume) != 0;
                  onEnded(shouldResume);
                }
              }];
  return self;
}

- (void)dealloc {
  [self invalidate];
}

- (void)invalidate {
  if (_token) {
    [[NSNotificationCenter defaultCenter] removeObserver:_token];
    _token = nil;
  }
}

@end

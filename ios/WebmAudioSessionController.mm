#import "WebmAudioSessionController.h"

#import <AVFoundation/AVFoundation.h>

#include "common/MediaLog.h"

@implementation WebmAudioSessionController

- (void)activate {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* err = nil;
  if (![session setCategory:AVAudioSessionCategoryPlayback
                       mode:AVAudioSessionModeDefault
                    options:0
                      error:&err]) {
    MEDIA_LOG_E("WebmAudioSessionController: set category failed: %s",
                err.localizedDescription.UTF8String);
  }
  if (![session setActive:YES error:&err]) {
    MEDIA_LOG_E("WebmAudioSessionController: activate failed: %s",
                err.localizedDescription.UTF8String);
  }
}

- (void)deactivate {
  NSError* err = nil;
  // Non-mixable Playback interrupted other apps' audio at activate; without
  // this handshake they never receive the hint that they may resume.
  if (![[AVAudioSession sharedInstance]
            setActive:NO
          withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                error:&err]) {
    MEDIA_LOG_W("WebmAudioSessionController: deactivate failed: %s",
                err.localizedDescription.UTF8String);
  }
}

@end

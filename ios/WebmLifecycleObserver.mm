#import "WebmLifecycleObserver.h"

#import <UIKit/UIKit.h>

@implementation WebmLifecycleObserver {
  id _backgroundToken;
  id _foregroundToken;
}

- (instancetype)initWithOnBackground:(void (^)(void))onBackground
                        onForeground:(void (^)(void))onForeground {
  self = [super init];
  if (!self) return nil;

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  _backgroundToken =
      [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                          object:nil
                           queue:[NSOperationQueue mainQueue]
                      usingBlock:^(NSNotification*) {
                        onBackground();
                      }];
  _foregroundToken =
      [center addObserverForName:UIApplicationWillEnterForegroundNotification
                          object:nil
                           queue:[NSOperationQueue mainQueue]
                      usingBlock:^(NSNotification*) {
                        onForeground();
                      }];
  return self;
}

- (void)dealloc {
  [self invalidate];
}

- (void)invalidate {
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  if (_backgroundToken) {
    [center removeObserver:_backgroundToken];
    _backgroundToken = nil;
  }
  if (_foregroundToken) {
    [center removeObserver:_foregroundToken];
    _foregroundToken = nil;
  }
}

@end

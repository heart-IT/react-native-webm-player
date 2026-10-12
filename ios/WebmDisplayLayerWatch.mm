#import "WebmDisplayLayerWatch.h"

#include "common/MediaLog.h"

@implementation WebmDisplayLayerWatch {
  AVSampleBufferDisplayLayer* _layer;
  id _flushToken;
  id _failureToken;
}

- (instancetype)initWithLayer:(AVSampleBufferDisplayLayer*)layer
                onFlushNeeded:(void (^)(void))onFlushNeeded {
  self = [super init];
  if (!self) return nil;
  _layer = layer;

  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  _flushToken = [center
      addObserverForName:AVSampleBufferDisplayLayerRequiresFlushToResumeDecodingDidChangeNotification
                  object:layer
                   queue:NSOperationQueue.mainQueue
              usingBlock:^(NSNotification*) {
                if (layer.requiresFlushToResumeDecoding) onFlushNeeded();
              }];
  _failureToken = [center
      addObserverForName:AVSampleBufferDisplayLayerFailedToDecodeNotification
                  object:layer
                   queue:NSOperationQueue.mainQueue
              usingBlock:^(NSNotification* note) {
                NSError* error =
                    note.userInfo[AVSampleBufferDisplayLayerFailedToDecodeNotificationErrorKey];
                MEDIA_LOG_E("WebmDisplayLayerWatch: layer failed to decode: %s",
                            error.description.UTF8String ?: "no error");
              }];
  return self;
}

- (void)dealloc {
  [self invalidate];
}

- (void)invalidate {
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  if (_flushToken) [center removeObserver:_flushToken];
  if (_failureToken) [center removeObserver:_failureToken];
  _flushToken = nil;
  _failureToken = nil;
}

- (void)logPerformance {
  if (@available(iOS 17.4, *)) {
    [_layer.sampleBufferRenderer
        loadVideoPerformanceMetricsWithCompletionHandler:^(AVVideoPerformanceMetrics* metrics) {
          if (!metrics) return;
          MEDIA_LOG_I("WebmDisplayLayerWatch: layer displayed %ld frames, dropped %ld, "
                      "corrupted %ld, accumulated delay %.3fs",
                      (long)metrics.totalNumberOfFrames, (long)metrics.numberOfDroppedFrames,
                      (long)metrics.numberOfCorruptedFrames, metrics.totalAccumulatedFrameDelay);
        }];
  }
}

@end

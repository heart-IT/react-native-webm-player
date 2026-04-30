// React Native bridge module exposing the V2 spike to JS.
// Method: V2SpikeBridge.runV2Spike() → Promise<{ result: 'played'|'dismissed'|'failed', error?: string }>
#import <React/RCTBridgeModule.h>
#import <UIKit/UIKit.h>

#import "V2Spike.h"

@interface V2SpikeBridge : NSObject <RCTBridgeModule>
@end

@implementation V2SpikeBridge

RCT_EXPORT_MODULE();

+ (BOOL)requiresMainQueueSetup { return NO; }

- (dispatch_queue_t)methodQueue { return dispatch_get_main_queue(); }

RCT_EXPORT_METHOD(runV2Spike:(RCTPromiseResolveBlock)resolve
                       reject:(RCTPromiseRejectBlock)reject) {
    UIViewController* root = [self topPresentedViewController];
    if (!root) {
        reject(@"no_root_vc", @"No presenting view controller available", nil);
        return;
    }

    V2SpikeViewController* vc = [[V2SpikeViewController alloc] init];
    vc.modalPresentationStyle = UIModalPresentationFullScreen;
    vc.completionHandler = ^(V2SpikeResult result, NSError* error) {
        NSString* resultStr = (result == V2SpikeResultPlayed) ? @"played"
                            : (result == V2SpikeResultDismissed) ? @"dismissed"
                            : @"failed";
        if (error) {
            resolve(@{ @"result": resultStr, @"error": error.localizedDescription ?: @"" });
        } else {
            resolve(@{ @"result": resultStr });
        }
    };
    [root presentViewController:vc animated:YES completion:nil];
}

- (UIViewController*)topPresentedViewController {
    UIWindow* window = nil;
    for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        if (scene.activationState != UISceneActivationStateForegroundActive) continue;
        UIWindowScene* ws = (UIWindowScene*)scene;
        for (UIWindow* w in ws.windows) {
            if (w.isKeyWindow) { window = w; break; }
        }
        if (window) break;
    }
    UIViewController* vc = window.rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    return vc;
}

@end

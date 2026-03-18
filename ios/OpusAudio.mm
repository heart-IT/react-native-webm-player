#import "OpusAudio.h"
#import "JSIInstaller.h"
#import <React/RCTBridge+Private.h>
#import <React/RCTBridgeProxy+Cxx.h>
#import <ReactCommon/CallInvoker.h>
#import <jsi/jsi.h>

@interface RCTBridgeProxy (Runtime)
@property (nonatomic, readonly) void *runtime;
@end

@implementation WebmPlayer {
    BOOL _installed;
    facebook::jsi::Runtime* _runtime;
}

@synthesize bridge = _bridge;

RCT_EXPORT_MODULE()

+ (BOOL)requiresMainQueueSetup {
    return NO;
}

- (void)initialize {
    _installed = [self doInstall];
}

- (BOOL)doInstall {
    RCTBridge* bridge = self.bridge;
    if (!bridge) {
        return NO;
    }

    facebook::jsi::Runtime* runtime = nullptr;
    std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr;

    if ([[bridge class] isSubclassOfClass:[RCTBridgeProxy class]]) {
        RCTBridgeProxy* proxy = (RCTBridgeProxy*)bridge;
        runtime = (facebook::jsi::Runtime*)proxy.runtime;
        callInvoker = proxy.jsCallInvoker;
    } else {
        RCTCxxBridge* cxxBridge = (RCTCxxBridge*)bridge;
        runtime = (facebook::jsi::Runtime*)cxxBridge.runtime;
        NSLog(@"WebmPlayer: install failed — legacy RCTCxxBridge does not provide callInvoker (requires New Architecture)");
        return NO;
    }

    if (!runtime || !callInvoker) {
        NSLog(@"WebmPlayer: install failed (runtime=%p callInvoker=%d)",
              static_cast<void*>(runtime), callInvoker != nullptr);
        return NO;
    }

    _runtime = runtime;
    return installJSIInstaller(*runtime, callInvoker);
}

- (void)invalidate {
    if (_installed && _runtime) {
        uninstallJSIInstaller(*_runtime);
        _runtime = nullptr;
        _installed = NO;
    }
}

RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(install) {
    if (!_installed) {
        _installed = [self doInstall];
    }
    return @(_installed);
}

@end

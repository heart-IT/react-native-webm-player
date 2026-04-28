#import "WebmPlayer.h"
#import "JSIInstaller.h"
#import <React/RCTBridge+Private.h>
#import <React/RCTBridgeProxy+Cxx.h>
#import <ReactCommon/CallInvoker.h>
#import <jsi/jsi.h>
#include "../cpp/common/MediaLog.h"

@interface RCTBridgeProxy (Runtime)
@property (nonatomic, readonly) void *runtime;
@end

@implementation WebmPlayer {
    // _installed and _runtime are mutated from both install() (JS thread) and
    // invalidate() (module queue / host teardown thread). All mutations go
    // through @synchronized(self) so the install-vs-invalidate window is closed.
    BOOL _installed;
    facebook::jsi::Runtime* _runtime;
}

@synthesize bridge = _bridge;

RCT_EXPORT_MODULE()

+ (BOOL)requiresMainQueueSetup {
    return NO;
}

// Synchronous install — invoked from JS to wire JSI host functions onto the
// runtime. Returns @(YES) on success, @(NO) if the bridge isn't the expected
// New-Architecture RCTBridgeProxy.
- (NSNumber *)install {
    @synchronized(self) {
        if (_installed) return @(YES);

        RCTBridge* bridge = self.bridge;
        if (!bridge) {
            MEDIA_LOG_E("WebmPlayer: install failed — self.bridge is nil");
            return @(NO);
        }

        // New Architecture is required (RN 0.81+). Legacy RCTCxxBridge does not
        // expose callInvoker, so install is unreachable on that path.
        if (![[bridge class] isSubclassOfClass:[RCTBridgeProxy class]]) {
            MEDIA_LOG_E("WebmPlayer: install failed — requires RN New Architecture (RCTBridgeProxy)");
            return @(NO);
        }

        RCTBridgeProxy* proxy = (RCTBridgeProxy*)bridge;
        auto* runtime = (facebook::jsi::Runtime*)proxy.runtime;
        auto callInvoker = proxy.jsCallInvoker;

        if (!runtime || !callInvoker) {
            MEDIA_LOG_E("WebmPlayer: install failed (runtime=%p callInvoker=%d)",
                        static_cast<void*>(runtime), callInvoker != nullptr);
            return @(NO);
        }

        _runtime = runtime;
        _installed = installJSIInstaller(*runtime, callInvoker);
        return @(_installed);
    }
}

// RCTInvalidating hook — called before the host's JS runtime is torn down.
// Uninstalls JSI host functions so the module has no dangling runtime pointer.
// Per RCTInvalidating contract, may be called on any thread — guard with
// @synchronized to serialize against install().
- (void)invalidate {
    @synchronized(self) {
        if (_installed && _runtime) {
            uninstallJSIInstaller(*_runtime);
            _runtime = nullptr;
            _installed = NO;
        }
    }
}

// TurboModule C++ wrapper factory — required for codegen'd specs so the framework
// can bridge sync Obj-C methods through JSI without the legacy RCT_EXPORT macros.
- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:
    (const facebook::react::ObjCTurboModule::InitParams &)params {
    return std::make_shared<facebook::react::NativeWebmPlayerSpecJSI>(params);
}

@end

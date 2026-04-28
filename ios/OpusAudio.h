// TurboModule entry point for the WebM player. Conforms to the codegen-generated
// NativeWebmPlayerSpec protocol (from src/specs/NativeWebmPlayer.ts) and exposes a
// single synchronous `install()` method that wires JSI host functions onto the JS
// runtime. All subsequent communication uses raw JSI — this module is touched only
// once per app session.
#import <RNWebmPlayerSpec/RNWebmPlayerSpec.h>
#import <React/RCTInvalidating.h>

@interface WebmPlayer : NSObject <NativeWebmPlayerSpec, RCTInvalidating>

@end

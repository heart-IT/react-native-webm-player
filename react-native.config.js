// Our native code is built by the library's own `externalNativeBuild` in
// android/build.gradle — NOT via autolinking's `cmakeListsPath`. Declaring
// cmakeListsPath would cause double-include of `ReactNative-application.cmake`
// because our CMakeLists follows the app-style setup.
//
// componentDescriptors is omitted because we don't ship Fabric components yet;
// ViewManagers run through the Fabric ViewManager Interop layer in RN 0.81+.
module.exports = {
  dependency: {
    platforms: {
      android: {}
    }
  }
}

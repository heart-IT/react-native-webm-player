# WebmPlayerExample

Example React Native app demonstrating `@heartit/webm-player` — live WebM broadcast playback with VP9 video and Opus audio.

## Prerequisites

- React Native development environment ([setup guide](https://reactnative.dev/docs/set-up-your-environment))
- Dependencies installed from project root: `yarn`

## Running

From the **project root** (not this directory):

```sh
# iOS
yarn example ios

# Android
yarn example android
```

Or start Metro separately:

```sh
yarn example start
```

Then build from Xcode (`example/ios/WebmPlayerExample.xcworkspace`) or Android Studio (`example/android`).

## iOS: Install CocoaPods

On first clone or after updating native dependencies:

```sh
cd example/ios && pod install && cd ../..
```

## Editing Native Code

- **iOS:** Open `example/ios/WebmPlayerExample.xcworkspace` in Xcode. Library sources are at `Pods > Development Pods > webm-player`.
- **Android:** Open `example/android` in Android Studio. Library sources are under `webm-player`.

Changes to TypeScript reflect immediately via Fast Refresh. Native C++ changes require a rebuild.

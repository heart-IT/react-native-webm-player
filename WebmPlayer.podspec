require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "WebmPlayer"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  # iOS only: the vendored opus.xcframework carries no visionOS slice, so a
  # visionOS consumer would fail at link, not at pod install. The floor is
  # the consuming app's, above React Native's own min_ios_version_supported.
  s.platforms    = { :ios => "16.6" }
  s.source       = { :git => "https://github.com/heart-IT/react-native-webm-player.git", :tag => "#{s.version}" }

  # Build the Opus XCFramework from source during pod install, mirroring the
  # Android side's CMake FetchContent. Skipped when already built.
  s.prepare_command = <<-SCRIPT
    set -e
    if [ ! -f ios/opus/lib/opus.xcframework/Info.plist ]; then
      echo "[WebmPlayer] Building Opus XCFramework from source..."
      bash ios/opus/build-opus.sh build
    fi
  SCRIPT

  s.source_files = [
    # Implementation (Swift)
    "ios/**/*.{swift}",
    # Autolinking/Registration (Objective-C++)
    "ios/**/*.{m,mm}",
    # Implementation (C++ objects). `.h` is included deliberately: the shared
    # C++ uses .h headers, and omitting them here keeps CocoaPods from exposing
    # them to the compiler. `.cc` is required for vendored libwebm mkvparser,
    # which would otherwise be silently skipped and fail at link time.
    "cpp/**/*.{h,hpp,cpp,cc}",
  ]

  # The build tree and the builder script are inputs, not sources.
  s.exclude_files = ["ios/opus/build/**/*", "ios/opus/build-opus.sh"]

  s.vendored_frameworks = ["ios/opus/lib/opus.xcframework"]

  # The broadcast worklet (worklet/feeder.js packed by `npm run bundle:worklet`),
  # loaded by WebmWorkletSource from this resource bundle.
  s.resource_bundles = { "WebmPlayerWorklet" => ["worklet/dist/feeder.bundle"] }

  # AVFoundation: renderer/synchronizer + audio session. VideoToolbox: VP9
  # decompression sessions. CoreMedia/CoreVideo: sample and pixel buffers.
  s.frameworks = [
    "AVFoundation",
    "VideoToolbox",
    "CoreMedia",
    "CoreVideo",
  ]

  # Nitrogen pins public_header_files to its own generated headers, so anything
  # Swift must see has to be declared public here too. Appended before
  # add_nitrogen_files so the autolinking script preserves it.
  s.public_header_files = ["ios/WebmPlaybackEngine.h"]

  # BareKit.xcframework belongs to react-native-bare-kit, which the app depends
  # on directly: its pod embeds the framework and the worklet's addons. It is
  # not a pod dependency here because that pod defines no module map and
  # CocoaPods refuses such a dependency from a Swift pod. WebmWorkletSource.mm
  # finds the header through the framework search path instead, and the
  # framework is linked into the app by that pod.
  bare_kit = File.dirname(%x(node -p "require.resolve('react-native-bare-kit/package.json', {paths: ['#{__dir__}']})").strip)
  bare_kit_xcframework = "#{bare_kit}/ios/BareKit.xcframework"

  # The shared C++ includes are rooted at cpp/ ("common/MediaLog.h",
  # "demux/WebmDemuxer.h"), at the libwebm root ("mkvparser/mkvparser.h") and at
  # the Opus include dir ("opus.h"). add_nitrogen_files merges into this hash and
  # sets no conflicting key, so these survive.
  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)/cpp"',
      '"$(PODS_TARGET_SRCROOT)/cpp/third_party/libwebm"',
      '"$(PODS_TARGET_SRCROOT)/ios/opus/lib/include"',
      '"$(PODS_TARGET_SRCROOT)/ios/opus/lib/include/opus"',
    ].join(' '),
    # $(inherited) keeps the paths CocoaPods adds for the other pods: with a
    # prebuilt React Core, React's own headers come from a framework too.
    'FRAMEWORK_SEARCH_PATHS[sdk=iphoneos*]' => "$(inherited) \"#{bare_kit_xcframework}/ios-arm64\"",
    'FRAMEWORK_SEARCH_PATHS[sdk=iphonesimulator*]' => "$(inherited) \"#{bare_kit_xcframework}/ios-arm64_x86_64-simulator\"",
    'OTHER_LDFLAGS' => '-framework BareKit',
  }

  load 'nitrogen/generated/ios/WebmPlayer+autolinking.rb'
  add_nitrogen_files(s)

  s.dependency 'React-jsi'
  s.dependency 'React-callinvoker'
  install_modules_dependencies(s)
end

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
  # visionOS consumer would fail at link, not at pod install.
  s.platforms    = { :ios => min_ios_version_supported }
  s.source       = { :git => "https://github.com/rahulgarg/react-native-webm-player.git", :tag => "#{s.version}" }

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

  # AVFoundation: renderer/synchronizer + audio session. AudioToolbox: LPCM
  # format descriptions. VideoToolbox: VP9 decompression sessions.
  # CoreMedia/CoreVideo: sample and pixel buffers.
  s.frameworks = [
    "AVFoundation",
    "AudioToolbox",
    "VideoToolbox",
    "CoreMedia",
    "CoreVideo",
  ]

  # Nitrogen pins public_header_files to its own generated headers, so anything
  # Swift must see has to be declared public here too. Appended before
  # add_nitrogen_files so the autolinking script preserves it.
  s.public_header_files = ["ios/WebmPlaybackEngine.h"]

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
  }

  load 'nitrogen/generated/ios/WebmPlayer+autolinking.rb'
  add_nitrogen_files(s)

  s.dependency 'React-jsi'
  s.dependency 'React-callinvoker'
  install_modules_dependencies(s)
end

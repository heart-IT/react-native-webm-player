require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "WebmPlayer"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => min_ios_version_supported, :visionos => 1.0 }
  s.source       = { :git => "https://github.com/rahulgarg/react-native-webm-player.git", :tag => "#{s.version}" }

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

  # The shared C++ includes are rooted at cpp/ ("common/MediaLog.h",
  # "demux/WebmDemuxer.h") and at the libwebm root ("mkvparser/mkvparser.h").
  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)/cpp"',
      '"$(PODS_TARGET_SRCROOT)/cpp/third_party/libwebm"',
    ].join(' '),
  }

  load 'nitrogen/generated/ios/WebmPlayer+autolinking.rb'
  add_nitrogen_files(s)

  s.dependency 'React-jsi'
  s.dependency 'React-callinvoker'
  install_modules_dependencies(s)
end

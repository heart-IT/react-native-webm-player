require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "WebmPlayer"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => "15.1" }
  s.source       = { :git => "https://github.com/holepunchto/webm-player.git", :tag => "#{s.version}" }

  # iOS-specific sources (Objective-C++)
  # libwebm mkvparser sources vendored under cpp/third_party/libwebm/
  s.source_files = [
    "ios/**/*.{h,m,mm,cpp}",
    "cpp/**/*.{h,hpp,cpp}",
    "cpp/third_party/libwebm/mkvparser/*.{cc,h}",
    "cpp/third_party/libwebm/common/*.h"
  ]

  # Exclude build artifacts
  s.exclude_files = [
    "ios/opus/build/**/*",
    "ios/opus/build-opus.sh"
  ]

  s.private_header_files = [
    "ios/**/*.h",
    "cpp/**/*.h",
    "cpp/**/*.hpp",
    "cpp/third_party/libwebm/**/*.h"
  ]

  # Required frameworks for audio, video, and SIMD
  s.frameworks = ["AVFoundation", "AudioToolbox", "VideoToolbox", "CoreMedia", "Accelerate", "UIKit"]

  # Vendored Opus 1.6.1 XCFramework
  s.vendored_frameworks = "ios/opus/lib/opus.xcframework"

  # Header search paths for cross-platform code and vendored Opus
  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '$(PODS_TARGET_SRCROOT)/cpp',
      '$(PODS_TARGET_SRCROOT)/cpp/common',
      '$(PODS_TARGET_SRCROOT)/cpp/playback',
      '$(PODS_TARGET_SRCROOT)/cpp/video',
      '$(PODS_TARGET_SRCROOT)/cpp/demux',
      '$(PODS_TARGET_SRCROOT)/cpp/third_party/libwebm',
      '$(PODS_TARGET_SRCROOT)/ios',
      '$(PODS_TARGET_SRCROOT)/ios/playback',
      '$(PODS_TARGET_SRCROOT)/ios/video',
      '$(PODS_TARGET_SRCROOT)/ios/opus/lib/include'
    ].join(' '),
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) HAVE_OPUS=1',
    # USE_NEON only on ARM — x86_64 simulator has no NEON registers
    'OTHER_CFLAGS[arch=arm64]' => '-DUSE_NEON=1 -fstack-protector-strong',
    # Hide internal symbols to prevent clashes with host app
    'GCC_SYMBOLS_PRIVATE_EXTERN' => 'YES',
    'GCC_INLINES_ARE_PRIVATE_EXTERN' => 'YES',
    # Warning flags for parity with Android CMake -Wall -Wextra -Werror=...
    'WARNING_CFLAGS' => '-Wall -Wextra -Wconversion -Wshadow -Wno-unused-parameter -Werror=return-type -Werror=non-virtual-dtor -Werror=uninitialized -Werror=format-security -Werror=implicit-fallthrough',
    # Stack buffer overflow detection (parity with Android -fstack-protector-strong)
    'OTHER_CFLAGS' => '-fstack-protector-strong',
    # Compile-time + runtime buffer overflow detection for stdlib functions
    'GCC_PREPROCESSOR_DEFINITIONS[config=Release]' => '$(inherited) _FORTIFY_SOURCE=2',
    # ThinLTO for release builds (parity with Android CMake config)
    'LLVM_LTO[config=Release]' => 'YES_THIN'
  }

  install_modules_dependencies(s)
end

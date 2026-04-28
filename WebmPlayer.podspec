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
  s.source       = { :git => "https://github.com/heart-IT/react-native-webm-player.git", :tag => "#{s.version}" }

  # Build vendored XCFrameworks from source during pod install (parity with Android CMake FetchContent).
  # Requires: Xcode CLI tools, cmake (for whisper). Runs once per pod install/update.
  s.prepare_command = <<-SCRIPT
    set -e
    if [ ! -f ios/opus/lib/opus.xcframework/Info.plist ]; then
      echo "[WebmPlayer] Building Opus 1.6.1 XCFramework from source..."
      bash ios/opus/build-opus.sh build
    fi
    if [ ! -f ios/whisper/lib/whisper.xcframework/Info.plist ]; then
      echo "[WebmPlayer] Building whisper.cpp v1.8.4 XCFramework from source..."
      bash ios/whisper/build-whisper.sh build
    fi
  SCRIPT

  # iOS-specific sources (Objective-C++)
  # libwebm mkvparser sources vendored under cpp/third_party/libwebm/
  # SpeexDSP resampler sources vendored under cpp/third_party/speexdsp/
  s.source_files = [
    "ios/**/*.{h,m,mm,cpp}",
    "cpp/**/*.{h,hpp,cpp}",
    "cpp/third_party/libwebm/mkvparser/*.{cc,h}",
    "cpp/third_party/libwebm/common/*.h",
    "cpp/third_party/speexdsp/libspeexdsp/resample.c",
    "cpp/third_party/speexdsp/libspeexdsp/*.h",
    "cpp/third_party/speexdsp/include/speex/*.h"
  ]

  # Exclude build artifacts and build scripts
  s.exclude_files = [
    "ios/opus/build/**/*",
    "ios/opus/build-opus.sh",
    "ios/whisper/build/**/*",
    "ios/whisper/build-whisper.sh",
    "ios/whisper/models/**/*"
  ]

  s.private_header_files = [
    "ios/**/*.h",
    "cpp/**/*.h",
    "cpp/**/*.hpp",
    "cpp/third_party/libwebm/**/*.h",
    "cpp/third_party/speexdsp/**/*.h"
  ]

  # Required frameworks for audio, video, SIMD, and Metal (whisper.cpp GPU inference)
  s.frameworks = ["AVFoundation", "AudioToolbox", "VideoToolbox", "CoreMedia", "Accelerate", "UIKit", "Metal"]

  # Vendored Opus 1.6.1 XCFramework + whisper.cpp XCFramework
  s.vendored_frameworks = ["ios/opus/lib/opus.xcframework", "ios/whisper/lib/whisper.xcframework"]

  # Header search paths for cross-platform code and vendored Opus
  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '$(PODS_TARGET_SRCROOT)/cpp',
      '$(PODS_TARGET_SRCROOT)/cpp/common',
      '$(PODS_TARGET_SRCROOT)/cpp/playback',
      '$(PODS_TARGET_SRCROOT)/cpp/video',
      '$(PODS_TARGET_SRCROOT)/cpp/demux',
      '$(PODS_TARGET_SRCROOT)/cpp/third_party/libwebm',
      '$(PODS_TARGET_SRCROOT)/cpp/third_party/speexdsp/include',
      '$(PODS_TARGET_SRCROOT)/cpp/third_party/speexdsp/libspeexdsp',
      '$(PODS_TARGET_SRCROOT)/ios',
      '$(PODS_TARGET_SRCROOT)/ios/playback',
      '$(PODS_TARGET_SRCROOT)/ios/video',
      '$(PODS_TARGET_SRCROOT)/ios/opus/lib/include',
      '$(PODS_TARGET_SRCROOT)/ios/opus/lib/include/opus',
      '$(PODS_TARGET_SRCROOT)/ios/whisper/lib/include',
      '$(PODS_TARGET_SRCROOT)/cpp/transcript'
    ].join(' '),
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) HAVE_OPUS=1 HAVE_WHISPER=1 FLOATING_POINT=1 EXPORT=',
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
    'LLVM_LTO[config=Release]' => 'YES_THIN',
    # -O3 + -funroll-loops to match Android CMake. Xcode default Release is -Os,
    # which under-vectorizes the NEON inner loops in AudioMixer + AudioLevelMeter.
    'GCC_OPTIMIZATION_LEVEL[config=Release]' => '3',
    'OTHER_CFLAGS[config=Release]' => '$(inherited) -funroll-loops'
  }

  install_modules_dependencies(s)
end

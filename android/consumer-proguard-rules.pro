# webm-player — JNI and React Native class preservation

# React Native module (discovered by reflection)
-keep class com.heartit.webmplayer.WebmPlayerPackage { *; }
-keep class com.heartit.webmplayer.WebmPlayerModule { *; }

# JNI bridge (native code calls static methods by name)
-keep class com.heartit.webmplayer.audio.AudioSessionBridge { *; }

# View managers (discovered by RN reflection; JNI binds native methods by name)
-keep class com.heartit.webmplayer.video.VideoViewManager { *; }
-keep class com.heartit.webmplayer.transcript.TranscriptViewManager { *; }

# Native code resolves onText(String) via GetMethodID at runtime
-keep class com.heartit.webmplayer.transcript.TranscriptViewManager$TextCallback { *; }

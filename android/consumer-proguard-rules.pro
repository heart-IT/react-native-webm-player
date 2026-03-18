# webm-player — JNI and React Native class preservation

# React Native module (discovered by reflection)
-keep class com.holepunchto.webmplayer.WebmPlayerPackage { *; }
-keep class com.holepunchto.webmplayer.WebmPlayerModule { *; }

# JNI bridge (native code calls static methods by name)
-keep class com.holepunchto.webmplayer.audio.AudioSessionBridge { *; }

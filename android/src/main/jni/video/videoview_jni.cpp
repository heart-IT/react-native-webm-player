#include <jni.h>
#include <android/native_window_jni.h>
#include "video/VideoSurfaceRegistry.h"
#include "common/MediaLog.h"

extern "C" {

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_video_VideoViewManager_nativeRegisterSurface(
    JNIEnv* env, jclass, jint participantId, jobject surface) {
    if (!surface) {
        MEDIA_LOG_W("videoview_jni: null surface for participant %d", participantId);
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        MEDIA_LOG_E("videoview_jni: failed to get ANativeWindow for participant %d", participantId);
        return;
    }

    // ANativeWindow_fromSurface returns +1 ref. registerSurface takes ownership.
    // If the same surface is already registered, registerSurface releases the extra ref.
    media::VideoSurfaceRegistry::instance().registerSurface(participantId, window);
    MEDIA_LOG_I("videoview_jni: registered surface for participant %d", participantId);
}

JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_video_VideoViewManager_nativeUnregisterSurface(
    JNIEnv*, jclass, jint participantId) {
    media::VideoSurfaceRegistry::instance().unregisterSurface(participantId);
    MEDIA_LOG_I("videoview_jni: unregistered surface for participant %d", participantId);
}

JNIEXPORT jintArray JNICALL
Java_com_holepunchto_webmplayer_video_VideoViewManager_nativeGetVideoResolution(
    JNIEnv* env, jclass, jint participantId) {
    auto [w, h] = media::VideoSurfaceRegistry::instance().getDecodedResolution(participantId);
    jintArray result = env->NewIntArray(2);
    jint buf[2] = { w, h };
    env->SetIntArrayRegion(result, 0, 2, buf);
    return result;
}

}  // extern "C"

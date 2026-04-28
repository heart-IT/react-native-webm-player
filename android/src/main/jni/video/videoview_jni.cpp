#include <jni.h>
#include <android/native_window_jni.h>
#include "video/VideoSurfaceRegistry.h"
#include "common/MediaLog.h"

extern "C" {

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_video_VideoViewManager_nativeRegisterSurface(
    JNIEnv* env, jclass, jobject surface) {
    if (!surface) {
        MEDIA_LOG_W("videoview_jni: null surface");
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        MEDIA_LOG_E("videoview_jni: failed to get ANativeWindow");
        return;
    }

    // ANativeWindow_fromSurface returns +1 ref. registerSurface takes ownership.
    media::VideoSurfaceRegistry::instance().registerSurface(window);
    MEDIA_LOG_I("videoview_jni: registered surface");
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_video_VideoViewManager_nativeUnregisterSurface(
    JNIEnv*, jclass) {
    media::VideoSurfaceRegistry::instance().unregisterSurface();
    MEDIA_LOG_I("videoview_jni: unregistered surface");
}

JNIEXPORT jintArray JNICALL
Java_com_heartit_webmplayer_video_VideoViewManager_nativeGetVideoResolution(
    JNIEnv* env, jclass) {
    auto [w, h] = media::VideoSurfaceRegistry::instance().getDecodedResolution();
    jintArray result = env->NewIntArray(2);
    if (!result) return nullptr;
    jint buf[2] = { w, h };
    env->SetIntArrayRegion(result, 0, 2, buf);
    return result;
}

}  // extern "C"

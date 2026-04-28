// JNI bridge for TranscriptView — registers/unregisters with TranscriptRegistry.
// Text updates are dispatched to the Android main looper via ALooper.

#include <jni.h>
#include <mutex>
#include <android/looper.h>
#include "transcript/TranscriptRegistry.h"
#include "common/MediaLog.h"

namespace {

struct TranscriptJniBridge {
    JavaVM* jvm = nullptr;
    jobject callbackRef = nullptr;  // GlobalRef to Kotlin callback
    jmethodID onTextMethod = nullptr;

    // Guards all three fields. Transcript-thread callback reads under shared semantics
    // (quick peek + snapshot), JNI setters write under exclusive semantics. std::mutex
    // is sufficient — callback fires at whisper cadence (~100-500ms), not RT.
    std::mutex mutex;

    // Caller must hold `mutex`.
    void clearLocked(JNIEnv* env) {
        if (callbackRef) {
            env->DeleteGlobalRef(callbackRef);
            callbackRef = nullptr;
        }
        onTextMethod = nullptr;
    }
};

TranscriptJniBridge g_bridge;

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_transcript_TranscriptViewManager_nativeRegisterTranscriptView(
    JNIEnv* env, jclass) {
    {
        std::lock_guard<std::mutex> lk(g_bridge.mutex);
        g_bridge.clearLocked(env);  // Release stale GlobalRef on re-register
        env->GetJavaVM(&g_bridge.jvm);
    }

    media::transcript::TranscriptRegistry::instance().setCallback(
        media::transcript::CallbackSlot::NativeView,
        [](const media::transcript::TranscriptSegment& seg) {
            // Snapshot callback state under the bridge mutex.
            JavaVM* jvm = nullptr;
            jobject callbackRef = nullptr;
            jmethodID onTextMethod = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_bridge.mutex);
                jvm = g_bridge.jvm;
                callbackRef = g_bridge.callbackRef;
                onTextMethod = g_bridge.onTextMethod;
            }
            if (!jvm || !callbackRef || !onTextMethod) return;

            JNIEnv* env = nullptr;
            bool attached = false;
            int status = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
            if (status == JNI_EDETACHED) {
                jvm->AttachCurrentThread(&env, nullptr);
                attached = true;
            }
            if (!env) return;

            jstring jtext = env->NewStringUTF(seg.text.c_str());
            env->CallVoidMethod(callbackRef, onTextMethod, jtext);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(jtext);

            if (attached) jvm->DetachCurrentThread();
        });

    MEDIA_LOG_I("TranscriptJNI: view registered");
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_transcript_TranscriptViewManager_nativeUnregisterTranscriptView(
    JNIEnv* env, jclass) {
    media::transcript::TranscriptRegistry::instance().clearCallback(
        media::transcript::CallbackSlot::NativeView);
    std::lock_guard<std::mutex> lk(g_bridge.mutex);
    g_bridge.clearLocked(env);
    MEDIA_LOG_I("TranscriptJNI: view unregistered");
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_transcript_TranscriptViewManager_nativeSetTextCallback(
    JNIEnv* env, jclass, jobject callback) {
    std::lock_guard<std::mutex> lk(g_bridge.mutex);
    g_bridge.clearLocked(env);
    if (!callback) return;

    g_bridge.callbackRef = env->NewGlobalRef(callback);
    jclass cls = env->GetObjectClass(callback);
    g_bridge.onTextMethod = env->GetMethodID(cls, "onText", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cls);

    if (!g_bridge.onTextMethod) {
        MEDIA_LOG_E("TranscriptJNI: onText method not found");
        g_bridge.clearLocked(env);
    }
}

}  // extern "C"

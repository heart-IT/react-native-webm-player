// JNI bridge between Kotlin StreamPlayerModule (v2) and the WebMStreamBuffer
// singleton in cpp/common/. JS feeds bytes via feedData(), Kotlin pushes via
// nativeFeedData → ring buffer; ExoPlayer's DataSource pulls via nativeReadSharedWebM.
//
// Singleton: one buffer shared across all StreamPlayerModule instances. Most apps
// have one player; multi-player support would require keying by RN runtime id.
#include <jni.h>
#include <cstdint>
#include <mutex>

#include "common/WebMStreamBuffer.h"

namespace {

media::WebMStreamBuffer& sharedBuffer() {
    static media::WebMStreamBuffer::Config cfg;
    cfg.minCapacityBytes = 4 * 1024 * 1024;
    cfg.producerStallMs = 2000;
    cfg.consumerStallMs = 2000;
    cfg.severeBackpressureRatio = 0.7;
    cfg.batchReadThreshold = 1024;
    cfg.shutdownGraceMs = 100;
    cfg.logMinIntervalMs = 30000;
    static media::WebMStreamBuffer buffer(16 * 1024 * 1024, cfg);
    return buffer;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_heartit_webmplayer_v2_StreamPlayerModule_nativeFeedData(JNIEnv* env, jobject,
                                                                  jbyteArray data,
                                                                  jint offset, jint length) {
    if (!data || length <= 0) return 0;
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return 0;
    size_t wrote = sharedBuffer().write(
        reinterpret_cast<const uint8_t*>(bytes + offset),
        static_cast<size_t>(length),
        /*isClusterBoundary*/ false);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return static_cast<jint>(wrote);
}

JNIEXPORT jint JNICALL
Java_com_heartit_webmplayer_v2_StreamPlayerModule_nativeReadSharedWebM(JNIEnv* env, jobject,
                                                                        jbyteArray dst,
                                                                        jint offset, jint length) {
    if (!dst || length <= 0) return 0;
    jbyte* bytes = env->GetByteArrayElements(dst, nullptr);
    if (!bytes) return 0;
    int got = sharedBuffer().read(reinterpret_cast<uint8_t*>(bytes + offset),
                                   static_cast<size_t>(length),
                                   /*timeoutMs*/ 50);
    env->ReleaseByteArrayElements(dst, bytes, 0);
    return static_cast<jint>(got);
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_v2_StreamPlayerModule_nativeGoToLive(JNIEnv*, jobject) {
    sharedBuffer().goToLive();
}

JNIEXPORT jboolean JNICALL
Java_com_heartit_webmplayer_v2_StreamPlayerModule_nativeIsBehindLive(JNIEnv*, jobject,
                                                                      jint thresholdBytes) {
    if (thresholdBytes <= 0) return JNI_FALSE;
    return sharedBuffer().isBehindLive(static_cast<size_t>(thresholdBytes)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_v2_StreamPlayerModule_nativeClear(JNIEnv*, jobject) {
    sharedBuffer().clear();
}

JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_v2_StreamPlayerModule_nativeSetEndOfStream(JNIEnv*, jobject) {
    sharedBuffer().setEndOfStream(true);
}

}  // extern "C"

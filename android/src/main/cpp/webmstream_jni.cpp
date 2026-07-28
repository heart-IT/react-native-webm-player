// JNI bridge between HybridWebmPlayer (Kotlin) and WebMStreamBuffer.
//
// Two deliberate differences from the predecessor's bridge:
//
//   1. The ring is per-instance, created and destroyed by the owning Kotlin
//      object. The old bridge used a process-wide singleton, so two players
//      would have silently shared one byte stream.
//
//   2. Writes take a direct ByteBuffer rather than a jbyteArray. Nitro hands
//      Kotlin a direct buffer wrapping the JS ArrayBuffer's memory, so
//      GetDirectBufferAddress reaches those bytes with no intermediate copy —
//      the only copy is the memcpy into the ring.
//
// The write path must complete before feedData() returns: a JS-created
// ArrayBuffer is non-owning and its memory is not valid afterwards.
#include <jni.h>

#include <cstdint>

#include "common/WebMStreamBuffer.h"

namespace {

media::WebMStreamBuffer* asBuffer(jlong handle) {
  return reinterpret_cast<media::WebMStreamBuffer*>(handle);
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeCreateRing(JNIEnv*, jobject,
                                                      jlong capacityBytes) {
  media::WebMStreamBuffer::Config cfg;
  cfg.minCapacityBytes = 4 * 1024 * 1024;
  cfg.producerStallMs = 2000;
  cfg.consumerStallMs = 2000;
  cfg.severeBackpressureRatio = 0.7;
  cfg.batchReadThreshold = 1024;
  cfg.shutdownGraceMs = 100;
  cfg.logMinIntervalMs = 30000;
  auto* buffer =
      new media::WebMStreamBuffer(static_cast<size_t>(capacityBytes), cfg);
  return reinterpret_cast<jlong>(buffer);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeDestroyRing(JNIEnv*, jobject,
                                                       jlong handle) {
  delete asBuffer(handle);
}

JNIEXPORT jint JNICALL Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeWrite(
    JNIEnv* env, jobject, jlong handle, jobject directBuffer, jint length) {
  auto* ring = asBuffer(handle);
  if (!ring || !directBuffer || length <= 0) return 0;
  auto* base =
      static_cast<const uint8_t*>(env->GetDirectBufferAddress(directBuffer));
  if (!base) return 0;  // not a direct buffer — caller must supply one
  jlong capacity = env->GetDirectBufferCapacity(directBuffer);
  if (capacity < static_cast<jlong>(length)) return 0;
  return static_cast<jint>(
      ring->write(base, static_cast<size_t>(length), /*isClusterBoundary=*/false));
}

// ExoPlayer's DataSource hands us a byte[], so this side cannot avoid the JNI
// array access. It runs on the loader thread, never on an audio callback.
JNIEXPORT jint JNICALL Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeRead(
    JNIEnv* env, jobject, jlong handle, jbyteArray dst, jint offset,
    jint length, jint timeoutMs) {
  auto* ring = asBuffer(handle);
  if (!ring || !dst || length <= 0) return 0;
  jbyte* bytes = env->GetByteArrayElements(dst, nullptr);
  if (!bytes) return 0;
  int got = ring->read(reinterpret_cast<uint8_t*>(bytes + offset),
                       static_cast<size_t>(length),
                       static_cast<uint64_t>(timeoutMs));
  env->ReleaseByteArrayElements(dst, bytes, 0);
  return static_cast<jint>(got);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeSetEndOfStream(JNIEnv*, jobject,
                                                          jlong handle) {
  if (auto* ring = asBuffer(handle)) ring->setEndOfStream(true);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeClear(JNIEnv*, jobject,
                                                 jlong handle) {
  if (auto* ring = asBuffer(handle)) ring->clear();
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeShutdown(JNIEnv*, jobject,
                                                    jlong handle) {
  if (auto* ring = asBuffer(handle)) ring->shutdown();
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeGoToLive(JNIEnv*, jobject,
                                                    jlong handle) {
  if (auto* ring = asBuffer(handle)) ring->goToLive();
}

JNIEXPORT jboolean JNICALL
Java_com_margelo_nitro_webmplayer_HybridWebmPlayer_nativeIsBehindLive(JNIEnv*, jobject,
                                                        jlong handle,
                                                        jint thresholdBytes) {
  auto* ring = asBuffer(handle);
  if (!ring || thresholdBytes <= 0) return JNI_FALSE;
  return ring->isBehindLive(static_cast<size_t>(thresholdBytes)) ? JNI_TRUE
                                                                 : JNI_FALSE;
}

}  // extern "C"

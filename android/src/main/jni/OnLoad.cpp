#include <fbjni/fbjni.h>
#include "JniGlobals.h"

JavaVM* g_jvm = nullptr;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return facebook::jni::initialize(vm, [] {});
}

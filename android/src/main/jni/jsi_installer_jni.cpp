#include <jni.h>
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <ReactCommon/CallInvokerHolder.h>
#include <fbjni/fbjni.h>
#include "JSIInstaller.h"
#include "common/ClipIndex.h"

using namespace facebook;

extern "C" JNIEXPORT jboolean JNICALL
Java_com_heartit_webmplayer_WebmPlayerModule_nativeInstallJSI(
    JNIEnv* env,
    jobject thiz,
    jlong runtimePtr,
    jobject callInvokerHolder,
    jstring cacheDir
) {
    if (runtimePtr == 0) return JNI_FALSE;
    if (callInvokerHolder == nullptr) return JNI_FALSE;

    // Defensive: Kotlin side types this as the public CallInvokerHolder interface.
    // RN 0.81.5 always returns a CallInvokerHolderImpl (fbjni HybridClass), which
    // is what `cthis()` expects. If RN ever changes the concrete type, fail cleanly
    // here instead of dereferencing a non-hybrid object.
    jclass implClass = env->FindClass("com/facebook/react/turbomodule/core/CallInvokerHolderImpl");
    if (!implClass) {
        env->ExceptionClear();
        return JNI_FALSE;
    }
    jboolean isImpl = env->IsInstanceOf(callInvokerHolder, implClass);
    env->DeleteLocalRef(implClass);
    if (!isImpl) return JNI_FALSE;

    auto& rt = *reinterpret_cast<jsi::Runtime*>(runtimePtr);

    auto holder = jni::make_local(
        reinterpret_cast<react::CallInvokerHolder::javaobject>(callInvokerHolder)
    );
    auto callInvoker = holder->cthis()->getCallInvoker();

    if (!callInvoker) return JNI_FALSE;

    // Set clip buffer temp directory from the Kotlin-supplied cache dir.
    // Reflecting android.app.ActivityThread.currentApplication() is a greylist API;
    // pushing the path down from ReactContext is both safer and simpler.
    if (cacheDir) {
        const char* path = env->GetStringUTFChars(cacheDir, nullptr);
        if (path && *path) media::ClipIndex::setTempDir(path);
        if (path) env->ReleaseStringUTFChars(cacheDir, path);
    }

    return installJSIInstaller(rt, callInvoker) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_heartit_webmplayer_WebmPlayerModule_nativeUninstallJSI(
    JNIEnv* env,
    jobject thiz,
    jlong runtimePtr
) {
    if (runtimePtr == 0) return;

    auto& rt = *reinterpret_cast<jsi::Runtime*>(runtimePtr);
    uninstallJSIInstaller(rt);
}

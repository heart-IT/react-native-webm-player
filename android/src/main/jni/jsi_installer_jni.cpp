#include <jni.h>
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <ReactCommon/CallInvokerHolder.h>
#include <fbjni/fbjni.h>
#include "JSIInstaller.h"

using namespace facebook;

extern "C" JNIEXPORT jboolean JNICALL
Java_com_holepunchto_webmplayer_WebmPlayerModule_nativeInstallJSI(
    JNIEnv* env,
    jobject thiz,
    jlong runtimePtr,
    jobject callInvokerHolder
) {
    if (runtimePtr == 0) return JNI_FALSE;
    if (callInvokerHolder == nullptr) return JNI_FALSE;

    auto& rt = *reinterpret_cast<jsi::Runtime*>(runtimePtr);

    auto holder = jni::make_local(
        reinterpret_cast<react::CallInvokerHolder::javaobject>(callInvokerHolder)
    );
    auto callInvoker = holder->cthis()->getCallInvoker();

    if (!callInvoker) return JNI_FALSE;

    return installJSIInstaller(rt, callInvoker) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_holepunchto_webmplayer_WebmPlayerModule_nativeUninstallJSI(
    JNIEnv* env,
    jobject thiz,
    jlong runtimePtr
) {
    if (runtimePtr == 0) return;

    auto& rt = *reinterpret_cast<jsi::Runtime*>(runtimePtr);
    uninstallJSIInstaller(rt);
}

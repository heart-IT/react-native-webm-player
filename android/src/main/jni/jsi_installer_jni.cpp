#include <jni.h>
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <ReactCommon/CallInvokerHolder.h>
#include <fbjni/fbjni.h>
#include "JSIInstaller.h"
#include "common/ClipBuffer.h"

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

    // Set clip buffer temp directory from app cache dir
    jclass contextClass = env->FindClass("android/app/ActivityThread");
    if (contextClass) {
        jmethodID currentApp = env->GetStaticMethodID(contextClass, "currentApplication",
                                                       "()Landroid/app/Application;");
        if (currentApp) {
            jobject app = env->CallStaticObjectMethod(contextClass, currentApp);
            if (app) {
                jmethodID getCacheDir = env->GetMethodID(
                    env->GetObjectClass(app), "getCacheDir", "()Ljava/io/File;");
                if (getCacheDir) {
                    jobject cacheDir = env->CallObjectMethod(app, getCacheDir);
                    if (cacheDir) {
                        jmethodID getPath = env->GetMethodID(
                            env->GetObjectClass(cacheDir), "getAbsolutePath",
                            "()Ljava/lang/String;");
                        if (getPath) {
                            auto pathStr = static_cast<jstring>(
                                env->CallObjectMethod(cacheDir, getPath));
                            if (pathStr) {
                                const char* path = env->GetStringUTFChars(pathStr, nullptr);
                                media::ClipBuffer::setTempDir(path);
                                env->ReleaseStringUTFChars(pathStr, path);
                            }
                        }
                        env->DeleteLocalRef(cacheDir);
                    }
                }
                env->DeleteLocalRef(app);
            }
        }
        env->DeleteLocalRef(contextClass);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

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

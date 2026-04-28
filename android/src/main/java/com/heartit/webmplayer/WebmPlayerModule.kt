package com.heartit.webmplayer

import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.annotations.ReactModule
import com.facebook.react.turbomodule.core.interfaces.CallInvokerHolder
import com.heartit.webmplayer.audio.AudioSessionBridge

@ReactModule(name = WebmPlayerModule.NAME)
class WebmPlayerModule(reactContext: ReactApplicationContext) :
    NativeWebmPlayerSpec(reactContext) {

    companion object {
        const val NAME = "WebmPlayer"

        init {
            System.loadLibrary("webmplayer")
        }
    }

    // install() runs on JS thread; invalidate() runs on module queue — different
    // threads. @Volatile provides the happens-before edge needed for the write
    // at end of install() to be observable to the read at start of invalidate().
    @Volatile
    private var installedRuntimePtr: Long = 0L

    override fun getName() = NAME

    @Synchronized
    override fun install(): Boolean {
        val context = reactApplicationContext
        AudioSessionBridge.initIfNeeded(context)

        val runtimePtr = context.javaScriptContextHolder?.get() ?: return false
        if (runtimePtr == 0L) return false

        // ReactContext.getJSCallInvokerHolder() is public API (RN 0.81+) —
        // no @OptIn(FrameworkAPI::class) needed, no catalystInstance access.
        // The underlying jobject is a CallInvokerHolderImpl; the C++ side
        // (jsi_installer_jni.cpp) casts via react::CallInvokerHolder::javaobject
        // which works for both the public interface and the FrameworkAPI impl.
        val callInvokerHolder = context.jsCallInvokerHolder ?: return false

        val cacheDir = context.cacheDir?.absolutePath ?: ""
        val ok = nativeInstallJSI(runtimePtr, callInvokerHolder, cacheDir)
        if (ok) {
            installedRuntimePtr = runtimePtr
        }
        return ok
    }

    // TurboModule lifecycle hook (replaces old-arch onCatalystInstanceDestroy).
    // Called during ReactHost teardown before the JS runtime is destroyed — safe
    // window to uninstall JSI host functions. @Synchronized serializes against
    // install() in case invalidate fires on a different thread.
    @Synchronized
    override fun invalidate() {
        val ptr = installedRuntimePtr
        if (ptr != 0L) {
            nativeUninstallJSI(ptr)
            installedRuntimePtr = 0L
        }
        AudioSessionBridge.destroy()
        super.invalidate()
    }

    private external fun nativeInstallJSI(
        runtimePtr: Long,
        callInvokerHolder: CallInvokerHolder,
        cacheDir: String
    ): Boolean
    private external fun nativeUninstallJSI(runtimePtr: Long)
}

package com.holepunchto.webmplayer

import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.common.annotations.FrameworkAPI
import com.facebook.react.turbomodule.core.CallInvokerHolderImpl
import com.holepunchto.webmplayer.audio.AudioSessionBridge

@OptIn(FrameworkAPI::class)
class WebmPlayerModule(reactContext: ReactApplicationContext) : ReactContextBaseJavaModule(reactContext) {

    companion object {
        const val NAME = "WebmPlayer"

        init {
            System.loadLibrary("webmplayer")
        }
    }

    private var installedRuntimePtr: Long = 0L

    override fun getName() = NAME

    @ReactMethod(isBlockingSynchronousMethod = true)
    fun install(): Boolean {
        val context = reactApplicationContext

        AudioSessionBridge.initIfNeeded(context)

        val runtimePtr = context.javaScriptContextHolder?.get() ?: return false
        if (runtimePtr == 0L) return false

        val callInvokerHolder = context.catalystInstance?.jsCallInvokerHolder as? CallInvokerHolderImpl
            ?: return false

        val ok = nativeInstallJSI(runtimePtr, callInvokerHolder)
        if (ok) {
            installedRuntimePtr = runtimePtr
        }
        return ok
    }

    override fun onCatalystInstanceDestroy() {
        val ptr = installedRuntimePtr
        if (ptr != 0L) {
            nativeUninstallJSI(ptr)
            installedRuntimePtr = 0L
        }
        AudioSessionBridge.destroy()
        super.onCatalystInstanceDestroy()
    }

    private external fun nativeInstallJSI(runtimePtr: Long, callInvokerHolder: CallInvokerHolderImpl): Boolean
    private external fun nativeUninstallJSI(runtimePtr: Long)
}

package com.heartit.webmplayer

import com.facebook.react.BaseReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.model.ReactModuleInfo
import com.facebook.react.module.model.ReactModuleInfoProvider
import com.facebook.react.uimanager.ViewManager
import com.heartit.webmplayer.transcript.TranscriptViewManager
import com.heartit.webmplayer.video.VideoViewManager

class WebmPlayerPackage : BaseReactPackage() {
    override fun getModule(name: String, reactContext: ReactApplicationContext): NativeModule? {
        return if (name == WebmPlayerModule.NAME) WebmPlayerModule(reactContext) else null
    }

    override fun getReactModuleInfoProvider(): ReactModuleInfoProvider {
        return ReactModuleInfoProvider {
            mapOf(
                WebmPlayerModule.NAME to ReactModuleInfo(
                    WebmPlayerModule.NAME,    // name
                    WebmPlayerModule.NAME,    // className
                    false,                    // canOverrideExistingModule
                    false,                    // needsEagerInit
                    false,                    // isCxxModule
                    true                      // isTurboModule
                )
            )
        }
    }

    // View managers stay on Paper/Interop — Fabric ViewManager Interop in RN 0.81.5
    // translates SimpleViewManager/RCTViewManager to Fabric component descriptors.
    override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> {
        return listOf(VideoViewManager(), TranscriptViewManager())
    }
}

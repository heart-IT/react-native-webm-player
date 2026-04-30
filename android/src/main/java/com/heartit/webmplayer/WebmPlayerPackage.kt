package com.heartit.webmplayer

import androidx.media3.common.util.UnstableApi
import com.facebook.react.BaseReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.model.ReactModuleInfo
import com.facebook.react.module.model.ReactModuleInfoProvider
import com.facebook.react.uimanager.ViewManager
import com.heartit.webmplayer.transcript.TranscriptViewManager
import com.heartit.webmplayer.v2.StreamPlayerModule
import com.heartit.webmplayer.v2.StreamPlayerViewManager
import com.heartit.webmplayer.video.VideoViewManager

@UnstableApi
class WebmPlayerPackage : BaseReactPackage() {
    override fun getModule(name: String, reactContext: ReactApplicationContext): NativeModule? {
        return when (name) {
            WebmPlayerModule.NAME -> WebmPlayerModule(reactContext)
            "WebmPlayerV2Module" -> StreamPlayerModule(reactContext)
            else -> null
        }
    }

    override fun getReactModuleInfoProvider(): ReactModuleInfoProvider {
        return ReactModuleInfoProvider {
            mapOf(
                WebmPlayerModule.NAME to ReactModuleInfo(
                    WebmPlayerModule.NAME, WebmPlayerModule.NAME,
                    false, false, false, true
                ),
                "WebmPlayerV2Module" to ReactModuleInfo(
                    "WebmPlayerV2Module", "WebmPlayerV2Module",
                    false, false, false, false  // legacy bridge module, not TurboModule
                )
            )
        }
    }

    override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> {
        return listOf(VideoViewManager(), TranscriptViewManager(), StreamPlayerViewManager())
    }
}

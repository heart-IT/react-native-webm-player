package com.webmplayer

import com.facebook.react.BaseReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.model.ReactModuleInfoProvider
import com.facebook.react.uimanager.ViewManager
import com.margelo.nitro.webmplayer.WebmPlayerOnLoad
import com.margelo.nitro.webmplayer.views.HybridWebmPlayerViewManager

class WebmPlayerPackage : BaseReactPackage() {
  override fun getModule(
    name: String,
    reactContext: ReactApplicationContext,
  ): NativeModule? = null

  override fun getReactModuleInfoProvider(): ReactModuleInfoProvider =
    ReactModuleInfoProvider { emptyMap() }

  /**
   * Nitro autolinks HybridObjects but not views: on Android a view manager only
   * reaches the registry if a package hands it over. Without this, rendering
   * <WebmPlayerView /> throws IllegalViewOperationException — and because the
   * HybridObject itself registers fine, the failure surfaces only on first
   * render, not at startup.
   */
  override fun createViewManagers(
    reactContext: ReactApplicationContext,
  ): List<ViewManager<*, *>> = listOf(HybridWebmPlayerViewManager())

  companion object {
    init {
      WebmPlayerOnLoad.initializeNative()
    }
  }
}

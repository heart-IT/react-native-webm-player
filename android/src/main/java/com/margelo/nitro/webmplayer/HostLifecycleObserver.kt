package com.margelo.nitro.webmplayer

import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.ReactApplicationContext

/**
 * App background/foreground, as two lambdas. Peer of iOS's WebmLifecycleObserver.
 *
 * Uses React Native's own host lifecycle rather than androidx ProcessLifecycleOwner,
 * so the library needs no extra dependency: onHostPause/onHostResume already track
 * exactly the transition we care about, and RN drives them from the Activity that
 * hosts the surface.
 *
 * onHostDestroy is treated as a background transition. It arrives when the host
 * activity is going away, and leaving playback running against a dying surface is
 * strictly worse than stopping it.
 */
class HostLifecycleObserver(
  private val context: ReactApplicationContext,
  private val onBackground: () -> Unit,
  private val onForeground: () -> Unit,
) : LifecycleEventListener {

  init {
    context.addLifecycleEventListener(this)
  }

  override fun onHostResume() = onForeground()

  override fun onHostPause() = onBackground()

  override fun onHostDestroy() = onBackground()

  fun invalidate() {
    context.removeLifecycleEventListener(this)
  }
}

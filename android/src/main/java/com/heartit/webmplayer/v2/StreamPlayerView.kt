package com.heartit.webmplayer.v2

import android.content.Context
import android.util.Log
import android.view.View
import android.widget.FrameLayout
import androidx.media3.common.util.UnstableApi
import androidx.media3.ui.PlayerView
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.ViewGroupManager
import com.facebook.react.uimanager.annotations.ReactProp

/**
 * V2 video view backed by ExoPlayer's PlayerView. Registers with
 * StreamPlayerModule on attach so JS can call attachToView(viewId) to bind
 * the player to this surface.
 */
@UnstableApi
class StreamPlayerView(context: Context) : FrameLayout(context) {
    private val playerView: PlayerView = PlayerView(context).apply {
        layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT)
        useController = false
        setShowBuffering(PlayerView.SHOW_BUFFERING_ALWAYS)
        setUseArtwork(false)
        setShutterBackgroundColor(0xFF000000.toInt())
    }
    private var viewId: Int = View.NO_ID

    init { addView(playerView) }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        viewId = id
        try { StreamPlayerModule.registerView(viewId, this) }
        catch (e: Exception) { Log.e("WebmPlayerV2View", "Register failed", e) }
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        try {
            StreamPlayerModule.unregisterView(viewId)
            playerView.player = null
        } catch (e: Exception) {
            Log.e("WebmPlayerV2View", "Unregister failed", e)
        }
    }

    fun setPlayer(player: androidx.media3.common.Player?) {
        playerView.player = player
        if (player != null) playerView.visibility = View.VISIBLE
        invalidate(); requestLayout()
    }

    fun setUseController(useController: Boolean) { playerView.useController = useController }
}

@UnstableApi
class StreamPlayerViewManager : ViewGroupManager<StreamPlayerView>() {
    override fun getName(): String = "WebmPlayerV2View"

    override fun createViewInstance(reactContext: ThemedReactContext): StreamPlayerView {
        return StreamPlayerView(reactContext)
    }

    @ReactProp(name = "useController", defaultBoolean = false)
    fun setUseController(view: StreamPlayerView, useController: Boolean) {
        view.setUseController(useController)
    }

    override fun onDropViewInstance(view: StreamPlayerView) {
        view.setPlayer(null)
        super.onDropViewInstance(view)
    }
}

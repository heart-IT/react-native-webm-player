package com.heartit.webmplayer.video

import android.view.Choreographer
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout
import com.facebook.react.uimanager.SimpleViewManager
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.annotations.ReactProp

class VideoViewManager : SimpleViewManager<FrameLayout>() {

    companion object {
        const val REACT_CLASS = "WebmVideoView"

        init {
            // Native library already loaded by WebmPlayerModule
        }

        @JvmStatic
        private external fun nativeRegisterSurface(surface: android.view.Surface)

        @JvmStatic
        private external fun nativeUnregisterSurface()

        @JvmStatic
        private external fun nativeGetVideoResolution(): IntArray
    }

    override fun getName(): String = REACT_CLASS

    override fun createViewInstance(reactContext: ThemedReactContext): FrameLayout {
        val container = FrameLayout(reactContext)
        val surfaceView = SurfaceView(reactContext)
        surfaceView.setZOrderMediaOverlay(true)
        container.addView(surfaceView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
            Gravity.CENTER
        ))
        container.setTag(TAG_SURFACE_VIEW, surfaceView)
        container.setTag(TAG_SCALE_MODE, 1) // default: fill
        return container
    }

    private fun getSurfaceView(container: FrameLayout): SurfaceView? =
        container.getTag(TAG_SURFACE_VIEW) as? SurfaceView

    private fun registerSurface(container: FrameLayout) {
        val surfaceView = getSurfaceView(container) ?: return

        val oldCallback = container.getTag(TAG_SURFACE_CALLBACK) as? SurfaceHolder.Callback
        if (oldCallback != null) {
            surfaceView.holder.removeCallback(oldCallback)
        }

        val callback = object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                nativeRegisterSurface(holder.surface)
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                nativeUnregisterSurface()
            }
        }

        container.setTag(TAG_SURFACE_CALLBACK, callback)
        // Set TAG_REGISTERED AFTER the immediate nativeRegisterSurface below
        // so surfaceCreated re-entrant calls can observe "not yet registered".
        surfaceView.holder.addCallback(callback)

        // addCallback does not replay surfaceCreated for an already-valid surface,
        // so we must register it once here. Guard against double-register via
        // TAG_REGISTERED in case the surfaceCreated callback also fires.
        val alreadyRegistered = container.getTag(TAG_REGISTERED) as? Boolean ?: false
        if (!alreadyRegistered && surfaceView.holder.surface != null && surfaceView.holder.surface.isValid) {
            nativeRegisterSurface(surfaceView.holder.surface)
        }
        container.setTag(TAG_REGISTERED, true)

        updateFitPolling(container)
    }

    @ReactProp(name = "mirror", defaultBoolean = false)
    fun setMirror(container: FrameLayout, mirror: Boolean) {
        val surfaceView = getSurfaceView(container) ?: return
        surfaceView.scaleX = if (mirror) -1f else 1f
    }

    @ReactProp(name = "scaleMode", defaultInt = 1)
    fun setScaleMode(container: FrameLayout, mode: Int) {
        container.setTag(TAG_SCALE_MODE, mode)
        applyScaleMode(container)
        updateFitPolling(container)
    }

    override fun onAfterUpdateTransaction(container: FrameLayout) {
        super.onAfterUpdateTransaction(container)
        // Register surface on first render
        val registered = container.getTag(TAG_REGISTERED) as? Boolean ?: false
        if (!registered) {
            registerSurface(container)
        }
    }

    override fun onDropViewInstance(container: FrameLayout) {
        stopFitPolling(container)
        val registered = container.getTag(TAG_REGISTERED) as? Boolean ?: false
        if (registered) {
            nativeUnregisterSurface()
            container.setTag(TAG_REGISTERED, false)
        }
        val surfaceView = getSurfaceView(container)
        val callback = container.getTag(TAG_SURFACE_CALLBACK) as? SurfaceHolder.Callback
        if (callback != null && surfaceView != null) {
            surfaceView.holder.removeCallback(callback)
        }
        super.onDropViewInstance(container)
    }

    private fun applyScaleMode(container: FrameLayout) {
        val surfaceView = getSurfaceView(container) ?: return
        val lp = surfaceView.layoutParams as? FrameLayout.LayoutParams ?: return
        val mode = container.getTag(TAG_SCALE_MODE) as? Int ?: 1

        if (mode == 0) {
            val containerW = container.width
            val containerH = container.height
            if (containerW > 0 && containerH > 0) {
                val res = nativeGetVideoResolution()
                val videoW = res[0]
                val videoH = res[1]
                if (videoW > 0 && videoH > 0) {
                    val videoAspect = videoW.toFloat() / videoH
                    val containerAspect = containerW.toFloat() / containerH

                    val svW: Int
                    val svH: Int
                    if (videoAspect > containerAspect) {
                        svW = containerW
                        svH = (containerW / videoAspect).toInt()
                    } else {
                        svH = containerH
                        svW = (containerH * videoAspect).toInt()
                    }

                    lp.width = svW
                    lp.height = svH
                    lp.gravity = Gravity.CENTER
                    surfaceView.layoutParams = lp
                    surfaceView.alpha = 1f
                    container.setTag(TAG_LAST_VIDEO_W, videoW)
                    container.setTag(TAG_LAST_VIDEO_H, videoH)
                    container.setTag(TAG_LAST_CONTAINER_W, containerW)
                    container.setTag(TAG_LAST_CONTAINER_H, containerH)
                    return
                }
            }
            // No resolution yet: keep the SurfaceView mounted so the decoder
            // can attach, but transparent to avoid the fill→letterbox flash
            // once the first keyframe lands (iOS parity via layer.videoGravity).
            lp.width = FrameLayout.LayoutParams.MATCH_PARENT
            lp.height = FrameLayout.LayoutParams.MATCH_PARENT
            lp.gravity = Gravity.CENTER
            surfaceView.layoutParams = lp
            surfaceView.alpha = 0f
        } else {
            lp.width = FrameLayout.LayoutParams.MATCH_PARENT
            lp.height = FrameLayout.LayoutParams.MATCH_PARENT
            lp.gravity = Gravity.CENTER
            surfaceView.layoutParams = lp
            surfaceView.alpha = 1f
            container.setTag(TAG_LAST_VIDEO_W, 0)
            container.setTag(TAG_LAST_VIDEO_H, 0)
        }
    }

    private fun updateFitPolling(container: FrameLayout) {
        val mode = container.getTag(TAG_SCALE_MODE) as? Int ?: 1

        if (mode == 0) {
            startFitPolling(container)
        } else {
            stopFitPolling(container)
            if (mode == 1) applyScaleMode(container)
        }
    }

    private fun startFitPolling(container: FrameLayout) {
        if (container.getTag(TAG_FIT_POLLING) != null) return

        val callback = object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (container.getTag(TAG_FIT_POLLING) == null) return

                val mode = container.getTag(TAG_SCALE_MODE) as? Int ?: 1
                if (mode != 0) {
                    container.setTag(TAG_FIT_POLLING, null)
                    return
                }

                val pollCount = (container.getTag(TAG_POLL_COUNTER) as? Int ?: 0) + 1
                if (pollCount < POLL_INTERVAL_FRAMES) {
                    container.setTag(TAG_POLL_COUNTER, pollCount)
                } else {
                    container.setTag(TAG_POLL_COUNTER, 0)

                    val res = nativeGetVideoResolution()
                    val videoW = res[0]
                    val videoH = res[1]
                    val containerW = container.width
                    val containerH = container.height
                    val lastVW = container.getTag(TAG_LAST_VIDEO_W) as? Int ?: 0
                    val lastVH = container.getTag(TAG_LAST_VIDEO_H) as? Int ?: 0
                    val lastCW = container.getTag(TAG_LAST_CONTAINER_W) as? Int ?: 0
                    val lastCH = container.getTag(TAG_LAST_CONTAINER_H) as? Int ?: 0

                    if (videoW != lastVW || videoH != lastVH || containerW != lastCW || containerH != lastCH) {
                        applyScaleMode(container)
                    }
                }

                Choreographer.getInstance().postFrameCallback(this)
            }
        }

        container.setTag(TAG_FIT_POLLING, callback)
        Choreographer.getInstance().postFrameCallback(callback)
    }

    private fun stopFitPolling(container: FrameLayout) {
        val callback = container.getTag(TAG_FIT_POLLING) as? Choreographer.FrameCallback
        if (callback != null) {
            Choreographer.getInstance().removeFrameCallback(callback)
            container.setTag(TAG_FIT_POLLING, null)
        }
    }
}

private const val TAG_REGISTERED = 0x7F000001
private const val TAG_SURFACE_CALLBACK = 0x7F000002
private const val TAG_SCALE_MODE = 0x7F000003
private const val TAG_SURFACE_VIEW = 0x7F000004
private const val TAG_FIT_POLLING = 0x7F000005
private const val TAG_LAST_VIDEO_W = 0x7F000006
private const val TAG_LAST_VIDEO_H = 0x7F000007
private const val TAG_LAST_CONTAINER_W = 0x7F000008
private const val TAG_LAST_CONTAINER_H = 0x7F000009
private const val TAG_POLL_COUNTER = 0x7F00000A
private const val POLL_INTERVAL_FRAMES = 15

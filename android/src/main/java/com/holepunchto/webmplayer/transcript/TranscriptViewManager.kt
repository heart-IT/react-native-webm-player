package com.holepunchto.webmplayer.transcript

import android.graphics.Color
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.TextView
import com.facebook.react.uimanager.SimpleViewManager
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.annotations.ReactProp

class TranscriptViewManager : SimpleViewManager<FrameLayout>() {

    companion object {
        const val REACT_CLASS = "WebmTranscriptView"
        private const val TAG_TEXT_VIEW = 0x7F100001
        private const val TAG_ENABLED = 0x7F100002

        @JvmStatic
        private external fun nativeRegisterTranscriptView()

        @JvmStatic
        private external fun nativeUnregisterTranscriptView()

        @JvmStatic
        private external fun nativeSetTextCallback(callback: TextCallback?)
    }

    interface TextCallback {
        fun onText(text: String)
    }

    override fun getName(): String = REACT_CLASS

    override fun createViewInstance(reactContext: ThemedReactContext): FrameLayout {
        val container = FrameLayout(reactContext)

        val textView = TextView(reactContext).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
            gravity = Gravity.CENTER
            setBackgroundColor(Color.argb(153, 0, 0, 0))
            setPadding(24, 12, 24, 12)
            visibility = android.view.View.GONE
        }

        val lp = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
        )
        lp.setMargins(32, 0, 32, 32)
        container.addView(textView, lp)
        container.setTag(TAG_TEXT_VIEW, textView)
        container.setTag(TAG_ENABLED, false)

        return container
    }

    @ReactProp(name = "enabled", defaultBoolean = false)
    fun setEnabled(container: FrameLayout, enabled: Boolean) {
        val textView = container.getTag(TAG_TEXT_VIEW) as? TextView ?: return
        val wasEnabled = container.getTag(TAG_ENABLED) as? Boolean ?: false
        container.setTag(TAG_ENABLED, enabled)

        if (enabled && !wasEnabled) {
            textView.visibility = android.view.View.VISIBLE
            val handler = Handler(Looper.getMainLooper())
            val callback = object : TextCallback {
                override fun onText(text: String) {
                    handler.post {
                        if (container.getTag(TAG_ENABLED) as? Boolean == true) {
                            textView.text = text
                            textView.visibility = if (text.isEmpty())
                                android.view.View.GONE else android.view.View.VISIBLE
                        }
                    }
                }
            }
            nativeSetTextCallback(callback)
            nativeRegisterTranscriptView()
        } else if (!enabled && wasEnabled) {
            nativeUnregisterTranscriptView()
            nativeSetTextCallback(null)
            textView.visibility = android.view.View.GONE
            textView.text = ""
        }
    }

    override fun onDropViewInstance(container: FrameLayout) {
        val wasEnabled = container.getTag(TAG_ENABLED) as? Boolean ?: false
        if (wasEnabled) {
            nativeUnregisterTranscriptView()
            nativeSetTextCallback(null)
        }
        super.onDropViewInstance(container)
    }
}

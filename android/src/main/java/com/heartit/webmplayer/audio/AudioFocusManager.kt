// Audio focus request/abandon — wraps the AudioManager.requestAudioFocus +
// AudioFocusRequest dance. Holds no state of its own; reads/writes
// audioFocusRequest on AudioSessionBridge so destroy() can call abandon()
// regardless of which path created the request.
package com.heartit.webmplayer.audio

import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.util.Log

internal object AudioFocusManager {
    fun request() {
        val am = AudioSessionBridge.audioManager ?: return
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
            .build()
        val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attrs)
            .setOnAudioFocusChangeListener { focusChange ->
                Log.i(AudioSessionBridge.TAG, "AudioFocus changed: $focusChange")
                AudioSessionBridge.fireNativeFocusChanged(focusChange)
            }
            .build()
        val result = am.requestAudioFocus(req)
        if (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            AudioSessionBridge.audioFocusRequest = req
            Log.i(AudioSessionBridge.TAG, "Audio focus granted")
        } else {
            Log.w(AudioSessionBridge.TAG, "Audio focus request failed: $result")
        }
    }

    fun abandon() {
        val am = AudioSessionBridge.audioManager ?: return
        AudioSessionBridge.audioFocusRequest?.let {
            am.abandonAudioFocusRequest(it)
            AudioSessionBridge.audioFocusRequest = null
            Log.i(AudioSessionBridge.TAG, "Audio focus abandoned")
        }
    }
}

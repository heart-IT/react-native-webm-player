// AudioSessionBridge.kt - Manages audio routing and Bluetooth for playback
package com.heartit.webmplayer.audio

import android.content.BroadcastReceiver
import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.util.Log
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner

object AudioSessionBridge : DefaultLifecycleObserver {
    internal const val TAG = "AudioSessionBridge"

    // Audio routes matching native enum
    object Route {
        const val UNKNOWN = 0
        const val EARPIECE = 1
        const val SPEAKER = 2
        const val WIRED_HEADSET = 3
        const val BLUETOOTH_SCO = 4
        const val BLUETOOTH_A2DP = 5
        const val USB_DEVICE = 6
    }

    // State accessed by helper objects in the same package (AudioFocusManager,
    // AudioRouteSetter, AudioDeviceQuery, AudioRouteReceivers). `internal`
    // confines visibility to this Gradle module — outside callers still see
    // only the @JvmStatic public API. @Volatile semantics are preserved for
    // cross-thread reads (BT broadcast receiver vs. JS thread).
    @Volatile internal var audioManager: AudioManager? = null
    @Volatile internal var context: Context? = null
    @Volatile private var initialized = false
    @Volatile internal var speakerPreferred = true  // Default to speaker on app start
    @Volatile internal var hasEarpiece = true  // Cached at init, false on tablets
    @Volatile internal var connectedBtDeviceName: String? = null
    @Volatile internal var preferredBtType: Int = AudioDeviceInfo.TYPE_BLUETOOTH_SCO
    @Volatile internal var audioFocusRequest: AudioFocusRequest? = null
    internal val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())

    internal var routeReceiver: BroadcastReceiver? = null
    internal var bluetoothReceiver: BroadcastReceiver? = null
    internal var communicationDeviceListener: Any? = null
    internal var audioDeviceCallback: AudioDeviceCallback? = null

    @JvmStatic
    @Synchronized
    fun initIfNeeded(context: Context) {
        if (initialized) return

        this.context = context.applicationContext
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager = am
        // Detect earpiece by checking actual audio output hardware, not FEATURE_TELEPHONY.
        // FEATURE_TELEPHONY indicates SIM/telephony support, not physical earpiece presence.
        // WiFi-only tablets with an earpiece speaker report FEATURE_TELEPHONY=false.
        hasEarpiece = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
        }

        nativeInit()

        registerRouteReceiver()
        registerBluetoothReceiver()
        registerAudioDeviceCallback()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            registerCommunicationDeviceListener()
        }
        registerLifecycleObserver()
        requestAudioFocus()

        initialized = true

        Log.d(TAG, "AudioSessionBridge initialized")
    }

    @JvmStatic
    @Synchronized
    fun destroy() {
        if (!initialized) return

        unregisterLifecycleObserver()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            unregisterCommunicationDeviceListener()
        }
        unregisterAudioDeviceCallback()
        unregisterRouteReceiver()
        unregisterBluetoothReceiver()

        // Cancel all pending mainHandler callbacks (BT restart, routeToBestDevice debounce)
        // BEFORE nativeDestroy() to prevent stale runnables from invoking JNI on destroyed
        // native state.
        mainHandler.removeCallbacksAndMessages(null)

        abandonAudioFocus()
        nativeDestroy()

        connectedBtDeviceName = null
        preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_SCO
        speakerPreferred = true
        audioManager = null
        context = null
        initialized = false

        Log.d(TAG, "AudioSessionBridge destroyed")
    }

    override fun onStart(owner: LifecycleOwner) {
        Log.i(TAG, "App foregrounded")
    }

    override fun onStop(owner: LifecycleOwner) {
        Log.i(TAG, "App backgrounded")
    }

    private fun registerLifecycleObserver() {
        try {
            ProcessLifecycleOwner.get().lifecycle.addObserver(this)
            Log.d(TAG, "Lifecycle observer registered")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to register lifecycle observer: ${e.message}")
        }
    }

    private fun unregisterLifecycleObserver() {
        try {
            ProcessLifecycleOwner.get().lifecycle.removeObserver(this)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to unregister lifecycle observer: ${e.message}")
        }
    }


    /**
     * Set the audio output route.
     * @param route One of Route.SPEAKER, Route.EARPIECE, Route.BLUETOOTH_SCO
     * @param deviceId Optional platform-specific device ID to target a specific device.
     *                 When empty, targets the first matching device for the route type.
     * @return true if route was set successfully
     */
    @JvmStatic
    fun setAudioRoute(route: Int, deviceId: String = ""): Boolean =
        AudioRouteSetter.setRoute(route, deviceId)

    /** Get list of available audio routes. */
    @JvmStatic
    fun getAvailableAudioRoutes(): IntArray = AudioDeviceQuery.getAvailableAudioRoutes()

    /** Get list of all available audio devices with names and IDs.
     *  Encoding: for each device, emit [route, deviceName, deviceId] as String[]. */
    @JvmStatic
    fun getAvailableAudioDevices(): Array<String> = AudioDeviceQuery.getAvailableAudioDevices()

    private fun registerBluetoothReceiver() = AudioRouteReceivers.registerBluetoothReceiver()

    private fun unregisterBluetoothReceiver() = AudioRouteReceivers.unregisterBluetoothReceiver()

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun registerCommunicationDeviceListener() = AudioRouteReceivers.registerCommunicationDeviceListener()

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun unregisterCommunicationDeviceListener() = AudioRouteReceivers.unregisterCommunicationDeviceListener()

    private fun registerAudioDeviceCallback() = AudioRouteReceivers.registerAudioDeviceCallback()

    private fun unregisterAudioDeviceCallback() = AudioRouteReceivers.unregisterAudioDeviceCallback()


    /**
     * Route to the best available device (API 31+).
     * Prefers external devices (BT, wired, USB) over builtin speaker/earpiece.
     */
    @JvmStatic
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    fun routeToBestDeviceNow() = AudioRouteSetter.routeToBestDeviceNow()

    @JvmStatic
    fun getCurrentRoute(): Int = AudioDeviceQuery.getCurrentRoute()

    private fun requestAudioFocus() = AudioFocusManager.request()

    private fun abandonAudioFocus() = AudioFocusManager.abandon()

    private fun registerRouteReceiver() = AudioRouteReceivers.registerRouteReceiver()

    private fun unregisterRouteReceiver() = AudioRouteReceivers.unregisterRouteReceiver()

    internal fun onRouteChanged() {
        val route = getCurrentRoute()
        val deviceId = AudioDeviceQuery.getCurrentRouteDeviceId()

        Log.i(TAG, "Route changed: ${AudioRoutePriority.routeToString(route)}")
        nativeOnAudioRouteChanged(route, deviceId)
    }

    internal fun fireNativeRouteChanged(route: Int, deviceId: String) {
        nativeOnAudioRouteChanged(route, deviceId)
    }

    internal fun fireNativeFocusChanged(focusChange: Int) {
        nativeOnAudioFocusChanged(focusChange)
    }

    private external fun nativeInit()
    private external fun nativeDestroy()
    private external fun nativeOnAudioRouteChanged(route: Int, deviceId: String)
    private external fun nativeOnAudioFocusChanged(focusChange: Int)
}

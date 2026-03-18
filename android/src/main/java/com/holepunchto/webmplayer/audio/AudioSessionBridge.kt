// AudioSessionBridge.kt - Manages audio session, AEC, focus, lifecycle, and Bluetooth
package com.holepunchto.webmplayer.audio

import android.Manifest
import android.bluetooth.BluetoothA2dp
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothHeadset
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import java.util.concurrent.atomic.AtomicInteger

object AudioSessionBridge : DefaultLifecycleObserver {
    private const val TAG = "AudioSessionBridge"

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

    // Echo modes matching native enum
    object EchoMode {
        const val HARDWARE_AEC = 0
        const val BLUETOOTH_AEC = 1
        const val NONE = 2
    }

    // Audio focus states matching native enum
    object FocusState {
        const val NONE = 0
        const val GAIN = 1
        const val LOSS = 2
        const val LOSS_TRANSIENT = 3
        const val LOSS_TRANSIENT_CAN_DUCK = 4
    }

    @Volatile private var audioManager: AudioManager? = null
    @Volatile private var context: Context? = null
    @Volatile private var initialized = false
    private var currentSessionId: Int = AudioManager.AUDIO_SESSION_ID_GENERATE
    @Volatile private var voiceModeEnabled = false
    @Volatile private var isForeground = true
    @Volatile private var hasAudioFocus = false
    @Volatile private var focusDelayedPending = false
    @Volatile private var speakerPreferred = true  // Default to speaker on app start
    @Volatile private var hasEarpiece = true  // Cached at init, false on tablets
    @Volatile private var connectedBtDeviceName: String? = null
    // User's preferred BT type for routeToBestDevice(). When both SCO and A2DP are
    // available, this determines which one auto-routing selects. Defaults to SCO
    // (bidirectional, lower latency for voice). Updated by explicit setAudioRoute() calls.
    @Volatile private var preferredBtType: Int = AudioDeviceInfo.TYPE_BLUETOOTH_SCO
    // Tracks pending legacy SCO connection attempt. Set before startBluetoothSco(),
    // cleared on ACTION_SCO_AUDIO_STATE_UPDATED (CONNECTED or DISCONNECTED).
    @Volatile private var scoConnectionPending = false
    // Monotonic counter incremented on each legacy SCO connection attempt.
    // Timeout lambdas capture the current value and only act if it still matches,
    // preventing stale timeouts from cancelling valid SCO connections.
    private val scoGeneration = AtomicInteger(0)
    private val focusLock = Any()
    private val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private val activeStreams = AtomicInteger(0)

    private var routeReceiver: BroadcastReceiver? = null
    private var bluetoothReceiver: BroadcastReceiver? = null
    private var audioFocusRequest: AudioFocusRequest? = null
    private var communicationDeviceListener: Any? = null
    private var audioDeviceCallback: AudioDeviceCallback? = null
    // Ensures JNI focus callbacks run on main thread
    private val audioFocusChangeListener = AudioManager.OnAudioFocusChangeListener { focusChange ->
        mainHandler.post {
            handleAudioFocusChange(focusChange)
        }
    }

    private fun handleAudioFocusChange(focusChange: Int) {
        synchronized(focusLock) {
            val state = when (focusChange) {
                AudioManager.AUDIOFOCUS_GAIN,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK -> {
                    hasAudioFocus = true
                    focusDelayedPending = false
                    FocusState.GAIN
                }
                AudioManager.AUDIOFOCUS_LOSS -> {
                    hasAudioFocus = false
                    focusDelayedPending = false
                    FocusState.LOSS
                }
                AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                    hasAudioFocus = false
                    FocusState.LOSS_TRANSIENT
                }
                AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                    FocusState.LOSS_TRANSIENT_CAN_DUCK
                }
                else -> FocusState.NONE
            }

            Log.i(TAG, "Audio focus changed: ${focusStateToString(state)}")
            nativeOnAudioFocusChanged(state)
        }
    }

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
        abandonAudioFocus()
        setVoiceMode(false)

        // Cancel all pending mainHandler callbacks (BT restart, SCO timeout,
        // routeToBestDevice debounce) BEFORE nativeDestroy() to prevent stale
        // runnables from invoking JNI on destroyed native state.
        mainHandler.removeCallbacksAndMessages(null)

        nativeDestroy()

        activeStreams.set(0)
        connectedBtDeviceName = null
        preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_SCO
        scoConnectionPending = false
        scoGeneration.set(0)
        speakerPreferred = true
        audioManager = null
        context = null
        initialized = false

        Log.d(TAG, "AudioSessionBridge destroyed")
    }

    /**
     * Called internally when a capture or playback stream starts.
     * On first stream: requests audio focus, enables voice mode, generates session ID.
     */
    @JvmStatic
    @Synchronized
    fun startSession() {
        if (!initialized) return

        val count = activeStreams.incrementAndGet()
        if (count == 1) {
            Log.i(TAG, "startSession: first stream, configuring audio session")
            // Re-attempt BT receiver registration if it was skipped at init
            // (BLUETOOTH_CONNECT wasn't granted yet). By startSession() time
            // the app should have requested permissions.
            if (bluetoothReceiver == null) {
                registerBluetoothReceiver()
            }
            queryConnectedBtDeviceName()
            setVoiceMode(true)
            if (!requestAudioFocus()) {
                nativeOnAudioFocusChanged(FocusState.LOSS_TRANSIENT)
            }
            generateSessionId()

            // Deferred BT restart: if BT is the active device at startup, the SCO/HFP
            // link may still be negotiating parameters (sample rate, codec). Schedule a
            // route re-detection after 1s so streams are restarted with the actual
            // negotiated parameters. Parity with iOS AudioSessionManager deferred restart.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val am = audioManager
                val currentDevice = am?.communicationDevice
                if (currentDevice != null && isBtType(currentDevice.type)) {
                    Log.i(TAG, "startSession: BT active at startup, scheduling deferred restart")
                    mainHandler.postDelayed({
                        if (initialized && activeStreams.get() > 0) {
                            Log.i(TAG, "startSession: executing deferred BT restart")
                            onRouteChanged()
                        }
                    }, 1000)
                }
            }
        } else {
            Log.i(TAG, "startSession: stream count=$count")
        }
    }

    /**
     * Called internally when a capture or playback stream stops.
     * On last stream: abandons audio focus, disables voice mode.
     */
    @JvmStatic
    @Synchronized
    fun stopSession() {
        if (!initialized) return

        val count = activeStreams.updateAndGet { c -> maxOf(0, c - 1) }
        if (count == 0) {
            Log.i(TAG, "stopSession: last stream, releasing audio session")
            abandonAudioFocus()
            setVoiceMode(false)
        } else {
            Log.i(TAG, "stopSession: stream count=$count")
        }
    }

    override fun onStart(owner: LifecycleOwner) {
        isForeground = true
        Log.i(TAG, "App foregrounded")
        nativeOnLifecycleChanged(true)

        // Xiaomi MIUI recovery is now handled by the C++ XiaomiModeResetQuirk
        // via LifecycleForeground event dispatched to the routing state machine.
    }

    override fun onStop(owner: LifecycleOwner) {
        isForeground = false
        Log.i(TAG, "App backgrounded")
        nativeOnLifecycleChanged(false)
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
     * Request audio focus for voice communication.
     * Call this before starting capture/playback.
     * Returns true if focus granted immediately, false if denied.
     * Use isFocusDelayedPending() to check if focus was delayed.
     */
    @JvmStatic
    fun requestAudioFocus(): Boolean {
        val am = audioManager ?: return false

        synchronized(focusLock) {
            if (hasAudioFocus) {
                Log.d(TAG, "Already have audio focus")
                return true
            }

            val attributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()

            audioFocusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attributes)
                .setAcceptsDelayedFocusGain(true)
                .setWillPauseWhenDucked(false)
                .setOnAudioFocusChangeListener(audioFocusChangeListener, mainHandler)
                .build()

            val result = am.requestAudioFocus(audioFocusRequest!!)

            when (result) {
                AudioManager.AUDIOFOCUS_REQUEST_GRANTED -> {
                    hasAudioFocus = true
                    focusDelayedPending = false
                    nativeOnAudioFocusChanged(FocusState.GAIN)
                    Log.i(TAG, "Audio focus request: granted")
                    return true
                }
                AudioManager.AUDIOFOCUS_REQUEST_DELAYED -> {
                    hasAudioFocus = false
                    focusDelayedPending = true
                    Log.i(TAG, "Audio focus request: delayed (will be granted later)")
                    return false
                }
                else -> {
                    hasAudioFocus = false
                    focusDelayedPending = false
                    Log.i(TAG, "Audio focus request: denied")
                    return false
                }
            }
        }
    }

    /**
     * Abandon audio focus.
     * Call this when stopping audio.
     */
    @JvmStatic
    fun abandonAudioFocus() {
        val am = audioManager ?: return

        synchronized(focusLock) {
            if (!hasAudioFocus && audioFocusRequest == null && !focusDelayedPending) return

            audioFocusRequest?.let {
                am.abandonAudioFocusRequest(it)
                audioFocusRequest = null
            }

            hasAudioFocus = false
            focusDelayedPending = false
            nativeOnAudioFocusChanged(FocusState.NONE)
            Log.i(TAG, "Audio focus abandoned")
        }
    }

    /**
     * Generate a session ID from AudioTrack for AEC correlation.
     * CRITICAL: Both input and output streams must use this session ID for AEC to work.
     */
    @JvmStatic
    fun generateSessionId(): Int {
        if (audioManager == null) return AudioManager.AUDIO_SESSION_ID_GENERATE

        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
            .build()

        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(48000)
            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
            .build()

        val minBufferSize = AudioTrack.getMinBufferSize(
            48000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        )

        val track = try {
            AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(format)
                .setBufferSizeInBytes(minBufferSize)
                .setSessionId(AudioManager.AUDIO_SESSION_ID_GENERATE)
                .build()
        } catch (e: Exception) {
            Log.w(TAG, "generateSessionId: AudioTrack.Builder failed: ${e.message}")
            return AudioManager.AUDIO_SESSION_ID_GENERATE
        }

        try {
            currentSessionId = track.audioSessionId
            nativeOnSessionIdGenerated(currentSessionId)
            Log.i(TAG, "Generated session ID: $currentSessionId")
            return currentSessionId
        } finally {
            track.release()
        }
    }

    /**
     * Enable voice communication mode for AEC/NS.
     * Call this before starting audio capture/playback.
     */
    @JvmStatic
    fun setVoiceMode(enable: Boolean) {
        val am = audioManager ?: return

        // Set voiceModeEnabled BEFORE routing so guards in AudioDeviceCallback
        // and BluetoothReceiver read the correct value if they fire during setup.
        voiceModeEnabled = enable

        if (enable) {
            am.mode = AudioManager.MODE_IN_COMMUNICATION

            // Route to the best available device: prefer connected BT/wired/USB over builtin.
            // Use routeToBestDeviceNow() — setup path needs immediate effect, not debounced.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                routeToBestDeviceNow()
            } else {
                @Suppress("DEPRECATION")
                if (am.isBluetoothScoOn) {
                    // SCO already active — keep it
                    am.isSpeakerphoneOn = false
                } else {
                    am.isSpeakerphoneOn = speakerPreferred
                }
            }
        } else {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                am.clearCommunicationDevice()
            }
            am.mode = AudioManager.MODE_NORMAL
        }

        nativeSetVoiceMode(enable)

        Log.i(TAG, "Voice mode: ${if (enable) "enabled" else "disabled"}, speaker=$speakerPreferred")
    }

    /**
     * Set the audio output route.
     * @param route One of Route.SPEAKER, Route.EARPIECE, Route.BLUETOOTH_SCO
     * @param deviceId Optional platform-specific device ID to target a specific device.
     *                 When empty, targets the first matching device for the route type.
     * @return true if route was set successfully
     */
    @JvmStatic
    fun setAudioRoute(route: Int, deviceId: String = ""): Boolean {
        val am = audioManager ?: return false

        if (!voiceModeEnabled) {
            Log.w(TAG, "setAudioRoute: called before voice mode enabled")
            return false
        }

        Log.i(TAG, "setAudioRoute: ${routeToString(route)}${if (deviceId.isNotEmpty()) " deviceId=$deviceId" else ""}")

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            setAudioRouteModern(am, route, deviceId)
        } else {
            setAudioRouteLegacy(am, route)
        }
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun setAudioRouteModern(am: AudioManager, route: Int, deviceId: String = ""): Boolean {
        val targetId = if (deviceId.isNotEmpty()) deviceId.toIntOrNull() else null

        val device = when (route) {
            Route.SPEAKER -> am.availableCommunicationDevices.firstOrNull {
                it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER &&
                    (targetId == null || it.id == targetId)
            }
            Route.EARPIECE -> am.availableCommunicationDevices.firstOrNull {
                it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE &&
                    (targetId == null || it.id == targetId)
            }
            Route.BLUETOOTH_SCO -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
                    it.type == AudioDeviceInfo.TYPE_BLE_HEADSET) &&
                    (targetId == null || it.id == targetId)
            }
            Route.BLUETOOTH_A2DP -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                    it.type == AudioDeviceInfo.TYPE_BLE_SPEAKER) &&
                    (targetId == null || it.id == targetId)
            }
            Route.WIRED_HEADSET -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                    it.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES) &&
                    (targetId == null || it.id == targetId)
            }
            Route.USB_DEVICE -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                    it.type == AudioDeviceInfo.TYPE_USB_HEADSET) &&
                    (targetId == null || it.id == targetId)
            }
            else -> null
        }

        if (device == null) {
            Log.w(TAG, "setAudioRoute: device for ${routeToString(route)} not available")
            return false
        }

        // Update state BEFORE setCommunicationDevice() so the
        // CommunicationDeviceChangedListener reads correct values when it fires.
        // Save previous values for rollback if setCommunicationDevice() fails.
        val prevSpeakerPreferred = speakerPreferred
        val prevPreferredBtType = preferredBtType
        if (route == Route.SPEAKER || route == Route.EARPIECE) {
            speakerPreferred = (route == Route.SPEAKER)
        }
        if (route == Route.BLUETOOTH_SCO) {
            preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_SCO
        } else if (route == Route.BLUETOOTH_A2DP) {
            preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
        }
        val success = am.setCommunicationDevice(device)
        if (success) {
            Log.i(TAG, "setAudioRoute: set to ${routeToString(route)}")
            // CommunicationDeviceChangedListener handles nativeOnAudioRouteChanged
        } else {
            Log.w(TAG, "setAudioRoute: setCommunicationDevice failed, rolling back state")
            speakerPreferred = prevSpeakerPreferred
            preferredBtType = prevPreferredBtType
        }
        return success
    }

    @Suppress("DEPRECATION")
    private fun setAudioRouteLegacy(am: AudioManager, route: Int): Boolean {
        when (route) {
            Route.SPEAKER -> {
                am.isSpeakerphoneOn = true
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                speakerPreferred = true
            }
            Route.EARPIECE -> {
                if (!hasEarpiece) {
                    Log.w(TAG, "setAudioRoute (legacy): earpiece not available on this device")
                    return false
                }
                am.isSpeakerphoneOn = false
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                speakerPreferred = false
            }
            Route.BLUETOOTH_SCO -> {
                am.isSpeakerphoneOn = false
                scoConnectionPending = true
                val gen = scoGeneration.incrementAndGet()
                am.startBluetoothSco()
                am.isBluetoothScoOn = true
                // SCO link takes 100-500ms to establish. Don't call onRouteChanged()
                // now — the route isn't active yet. ACTION_SCO_AUDIO_STATE_UPDATED
                // fires when SCO actually connects and triggers onRouteChanged() there.
                Log.i(TAG, "setAudioRoute (legacy): BT SCO requested, waiting for connection (gen=$gen)")
                // Fallback if SCO never connects (device out of range, BT stack error).
                // Uses generation counter to prevent stale timeouts from cancelling
                // valid SCO connections started by a subsequent setAudioRoute() call.
                mainHandler.postDelayed({
                    if (voiceModeEnabled && scoConnectionPending && scoGeneration.get() == gen) {
                        Log.w(TAG, "setAudioRoute (legacy): SCO connection timeout (gen=$gen), falling back")
                        scoConnectionPending = false
                        @Suppress("DEPRECATION")
                        am.stopBluetoothSco()
                        @Suppress("DEPRECATION")
                        am.isBluetoothScoOn = false
                        am.isSpeakerphoneOn = speakerPreferred
                        onRouteChanged()
                    }
                }, 4000)
                return true
            }
            Route.BLUETOOTH_A2DP -> {
                // Pre-API 31: no direct A2DP routing. Clear SCO and speakerphone
                // so the system routes to the connected A2DP device by default.
                if (!hasConnectedBluetoothA2dpDevice(am)) {
                    Log.w(TAG, "setAudioRoute (legacy): no A2DP device connected")
                    return false
                }
                am.isSpeakerphoneOn = false
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                speakerPreferred = false
                preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
            }
            Route.WIRED_HEADSET, Route.USB_DEVICE -> {
                // Pre-API 31: no setCommunicationDevice(). Clear SCO and speakerphone
                // so the system routes to the connected wired/USB device by default.
                if (!am.isWiredHeadsetOn && !hasConnectedUsbAudioDevice(am)) {
                    Log.w(TAG, "setAudioRoute (legacy): no wired/USB device connected")
                    return false
                }
                am.isSpeakerphoneOn = false
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                speakerPreferred = false
            }
            else -> return false
        }

        Log.i(TAG, "setAudioRoute (legacy): set to ${routeToString(route)}")
        // Post route notification asynchronously. On pre-API 31, setAudioRoute() may be
        // called from native via JNI while the C++ routingMutex_ is held. Calling
        // nativeOnAudioRouteChanged synchronously would re-acquire routingMutex_ on the
        // same thread (non-recursive mutex → deadlock). Posting to mainHandler defers the
        // notification until after the JNI call returns and routingMutex_ is released.
        mainHandler.post { onRouteChanged() }
        return true
    }

    private fun hasConnectedUsbAudioDevice(am: AudioManager): Boolean {
        return am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
        }
    }

    private fun hasConnectedBluetoothScoDevice(am: AudioManager): Boolean {
        return am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
        }
    }

    private fun hasConnectedBluetoothA2dpDevice(am: AudioManager): Boolean {
        return am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
        }
    }

    private fun isBtType(type: Int): Boolean = when (type) {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_HEADSET,
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> true
        else -> false
    }

    // Checks if the given device type is the BLE counterpart of the user's preferred BT type.
    // BLE_HEADSET is equivalent to SCO (bidirectional), BLE_SPEAKER to A2DP (output-only).
    private fun isPreferredBleCounterpart(type: Int): Boolean = when (preferredBtType) {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> type == AudioDeviceInfo.TYPE_BLE_HEADSET
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> type == AudioDeviceInfo.TYPE_BLE_SPEAKER
        else -> false
    }

    /**
     * Get list of available audio routes.
     * @return IntArray of available Route values
     */
    @JvmStatic
    fun getAvailableAudioRoutes(): IntArray {
        val am = audioManager ?: return intArrayOf()
        val routes = mutableListOf<Int>()

        // Earpiece only on phones (parity with iOS UIUserInterfaceIdiomPhone check)
        if (hasEarpiece) routes.add(Route.EARPIECE)
        routes.add(Route.SPEAKER)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            for (device in am.availableCommunicationDevices) {
                when (device.type) {
                    AudioDeviceInfo.TYPE_WIRED_HEADSET,
                    AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> {
                        if (!routes.contains(Route.WIRED_HEADSET)) routes.add(Route.WIRED_HEADSET)
                    }
                    AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> {
                        if (!routes.contains(Route.BLUETOOTH_SCO)) routes.add(Route.BLUETOOTH_SCO)
                    }
                    AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> {
                        if (!routes.contains(Route.BLUETOOTH_A2DP)) routes.add(Route.BLUETOOTH_A2DP)
                    }
                    AudioDeviceInfo.TYPE_USB_DEVICE,
                    AudioDeviceInfo.TYPE_USB_HEADSET -> {
                        if (!routes.contains(Route.USB_DEVICE)) routes.add(Route.USB_DEVICE)
                    }
                    AudioDeviceInfo.TYPE_BLE_HEADSET -> {
                        if (!routes.contains(Route.BLUETOOTH_SCO)) routes.add(Route.BLUETOOTH_SCO)
                    }
                    AudioDeviceInfo.TYPE_BLE_SPEAKER -> {
                        if (!routes.contains(Route.BLUETOOTH_A2DP)) routes.add(Route.BLUETOOTH_A2DP)
                    }
                }
            }
        } else {
            @Suppress("DEPRECATION")
            if (am.isWiredHeadsetOn) routes.add(Route.WIRED_HEADSET)
            if (hasConnectedBluetoothScoDevice(am)) routes.add(Route.BLUETOOTH_SCO)
            if (hasConnectedBluetoothA2dpDevice(am)) routes.add(Route.BLUETOOTH_A2DP)
            if (hasConnectedUsbAudioDevice(am)) routes.add(Route.USB_DEVICE)
        }

        return routes.toIntArray()
    }

    /**
     * Get list of all available audio devices with names and IDs.
     * Returns a flat array: [route0, nameLen0, name0..., id0, route1, ...].
     * The JNI layer unpacks this into AudioDeviceInfo structs.
     * Encoding: for each device, emit [route, deviceName, deviceId] as String[].
     */
    @JvmStatic
    fun getAvailableAudioDevices(): Array<String> {
        val am = audioManager ?: return emptyArray()
        val result = mutableListOf<String>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // On API 31+, enumerate all communication devices including built-ins.
            // This gives us real device IDs that match setCommunicationDevice().
            val seen = mutableSetOf<Int>()

            // Earpiece first (parity with iOS ordering)
            if (hasEarpiece) {
                for (device in am.availableCommunicationDevices) {
                    if (device.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE && device.id !in seen) {
                        seen.add(device.id)
                        result.add(Route.EARPIECE.toString())
                        result.add("Earpiece")
                        result.add(device.id.toString())
                        break
                    }
                }
            }

            // Speaker second
            for (device in am.availableCommunicationDevices) {
                if (device.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER && device.id !in seen) {
                    seen.add(device.id)
                    result.add(Route.SPEAKER.toString())
                    result.add("Speaker")
                    result.add(device.id.toString())
                    break
                }
            }

            // External devices
            for (device in am.availableCommunicationDevices) {
                if (device.id in seen) continue
                val route = when (device.type) {
                    AudioDeviceInfo.TYPE_WIRED_HEADSET,
                    AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> Route.WIRED_HEADSET
                    AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
                    AudioDeviceInfo.TYPE_BLE_HEADSET -> Route.BLUETOOTH_SCO
                    AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                    AudioDeviceInfo.TYPE_BLE_SPEAKER -> Route.BLUETOOTH_A2DP
                    AudioDeviceInfo.TYPE_USB_DEVICE,
                    AudioDeviceInfo.TYPE_USB_HEADSET -> Route.USB_DEVICE
                    else -> continue
                }
                seen.add(device.id)
                val name = resolveDeviceName(device, route)
                result.add(route.toString())
                result.add(name)
                result.add(device.id.toString())
            }
        } else {
            // Legacy: synthetic IDs (no per-device targeting available)
            if (hasEarpiece) {
                result.add(Route.EARPIECE.toString())
                result.add("Earpiece")
                result.add("builtin-earpiece")
            }

            result.add(Route.SPEAKER.toString())
            result.add("Speaker")
            result.add("builtin-speaker")

            @Suppress("DEPRECATION")
            if (am.isWiredHeadsetOn) {
                result.add(Route.WIRED_HEADSET.toString())
                result.add("Wired Headset")
                result.add("wired-headset")
            }
            if (hasConnectedBluetoothScoDevice(am)) {
                result.add(Route.BLUETOOTH_SCO.toString())
                result.add(connectedBtDeviceName ?: "Bluetooth SCO")
                result.add("bluetooth-sco")
            }
            if (hasConnectedBluetoothA2dpDevice(am)) {
                result.add(Route.BLUETOOTH_A2DP.toString())
                result.add(connectedBtDeviceName ?: "Bluetooth A2DP")
                result.add("bluetooth-a2dp")
            }
            if (hasConnectedUsbAudioDevice(am)) {
                result.add(Route.USB_DEVICE.toString())
                result.add("USB Device")
                result.add("usb-device")
            }
        }

        return result.toTypedArray()
    }

    private fun registerBluetoothReceiver() {
        val ctx = context ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "BLUETOOTH_CONNECT permission not granted, skipping Bluetooth receiver")
            return
        }

        bluetoothReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                when (intent?.action) {
                    BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED -> {
                        val state = intent.getIntExtra(
                            BluetoothProfile.EXTRA_STATE,
                            BluetoothProfile.STATE_DISCONNECTED
                        )
                        val connected = state == BluetoothProfile.STATE_CONNECTED
                        cacheBtDeviceName(intent, connected)
                        Log.i(TAG, "Bluetooth A2DP: ${if (connected) "connected" else "disconnected"}")
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            if (!voiceModeEnabled) return
                            if (connected) nativeOnDeviceConnected() else nativeOnDeviceDisconnected()
                        } else if (connected) {
                            onRouteChanged()
                        } else {
                            val fallbackRoute = if (speakerPreferred) Route.SPEAKER else Route.EARPIECE
                            setAudioRoute(fallbackRoute)
                        }
                    }
                    BluetoothHeadset.ACTION_CONNECTION_STATE_CHANGED -> {
                        val state = intent.getIntExtra(
                            BluetoothProfile.EXTRA_STATE,
                            BluetoothProfile.STATE_DISCONNECTED
                        )
                        val connected = state == BluetoothProfile.STATE_CONNECTED
                        cacheBtDeviceName(intent, connected)
                        Log.i(TAG, "Bluetooth SCO headset: ${if (connected) "connected" else "disconnected"}")
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            if (!voiceModeEnabled) return
                            if (connected) nativeOnDeviceConnected() else nativeOnDeviceDisconnected()
                        } else if (connected) {
                            onRouteChanged()
                        } else {
                            val fallbackRoute = if (speakerPreferred) Route.SPEAKER else Route.EARPIECE
                            setAudioRoute(fallbackRoute)
                        }
                    }
                    AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED -> {
                        val scoState = intent.getIntExtra(
                            AudioManager.EXTRA_SCO_AUDIO_STATE,
                            AudioManager.SCO_AUDIO_STATE_DISCONNECTED
                        )
                        Log.i(TAG, "Bluetooth SCO audio: ${scoAudioStateToString(scoState)}")

                        val wasPending = scoConnectionPending
                        if (scoState == AudioManager.SCO_AUDIO_STATE_CONNECTED ||
                            scoState == AudioManager.SCO_AUDIO_STATE_DISCONNECTED) {
                            scoConnectionPending = false
                        }

                        // API 31+: CommunicationDeviceChangedListener handles notification.
                        // Pre-31: need explicit route detection for SCO audio state changes.
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                            // SCO failed to connect — reset policy flag and fall back
                            if (scoState == AudioManager.SCO_AUDIO_STATE_DISCONNECTED && wasPending) {
                                Log.w(TAG, "Bluetooth SCO: connection failed, falling back")
                                val mgr = audioManager
                                if (mgr != null) {
                                    @Suppress("DEPRECATION")
                                    mgr.isBluetoothScoOn = false
                                    mgr.isSpeakerphoneOn = speakerPreferred
                                }
                            }
                            onRouteChanged()
                        }
                    }
                    BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED -> {
                        // API 31+: not actionable, CommunicationDeviceChangedListener handles.
                        // Pre-31: may indicate BT availability change.
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                            onRouteChanged()
                        }
                    }
                }
            }
        }

        val filter = IntentFilter().apply {
            addAction(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED)
            addAction(BluetoothHeadset.ACTION_CONNECTION_STATE_CHANGED)
            addAction(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED)
            addAction(BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            ctx.registerReceiver(bluetoothReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            ctx.registerReceiver(bluetoothReceiver, filter)
        }
        Log.d(TAG, "Bluetooth receiver registered")
    }

    private fun unregisterBluetoothReceiver() {
        val ctx = context ?: return
        bluetoothReceiver?.let {
            try {
                ctx.unregisterReceiver(it)
            } catch (e: Exception) {
                Log.w(TAG, "Error unregistering Bluetooth receiver: ${e.message}")
            }
        }
        bluetoothReceiver = null
    }

    /**
     * Register listener for communication device changes (API 31+).
     * This listener fires when setCommunicationDevice() takes effect, receiving
     * the actual AudioDeviceInfo. It is the single source of truth for route
     * notifications on API 31+, replacing all direct onRouteChanged() calls.
     */
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun registerCommunicationDeviceListener() {
        val am = audioManager ?: return

        val listener = AudioManager.OnCommunicationDeviceChangedListener { device ->
            // Samsung One UI 5.x+ sends spurious null during internal audio policy
            // reconfiguration. Verify by checking the actual communication device state —
            // if it's still set, the null is spurious and should be ignored.
            if (device == null) {
                val actual = am.communicationDevice
                if (actual != null) {
                    Log.i(TAG, "CommunicationDevice: spurious null (actual type=${actual.type}), ignoring")
                    return@OnCommunicationDeviceChangedListener
                }
            }

            val route = if (device != null) deviceTypeToRoute(device.type)
                else if (speakerPreferred) Route.SPEAKER else Route.EARPIECE
            val echoMode = getEchoModeForRoute(route)
            Log.i(TAG, "CommunicationDevice changed: ${routeToString(route)} (type=${device?.type})")

            val deviceId = device?.id?.toString() ?: ""

            // Dedup and restart decisions are handled by the C++ RouteHandler.
            nativeOnAudioRouteChanged(route, echoMode, deviceId)
        }
        am.addOnCommunicationDeviceChangedListener(mainHandler::post, listener)
        communicationDeviceListener = listener
        Log.d(TAG, "CommunicationDevice listener registered")
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun unregisterCommunicationDeviceListener() {
        val am = audioManager ?: return
        val listener = communicationDeviceListener as? AudioManager.OnCommunicationDeviceChangedListener
        if (listener != null) {
            am.removeOnCommunicationDeviceChangedListener(listener)
        }
        communicationDeviceListener = null
    }

    /**
     * Register AudioDeviceCallback for device availability changes.
     * Fires onAudioDevicesAdded/Removed when devices connect or disconnect from the system.
     * On API 31+: re-routes to the best device and notifies JS of updated device list.
     * Pre-31: notifies JS via onRouteChanged() so the device list refreshes.
     */
    private fun registerAudioDeviceCallback() {
        val am = audioManager ?: return

        val callback = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
                if (addedDevices.isNullOrEmpty()) return
                Log.i(TAG, "AudioDeviceCallback: ${addedDevices.size} device(s) added")

                if (!voiceModeEnabled) return

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    // Dispatch DeviceConnected to C++ state machine.
                    // SM calls routeToBestDevice via adapter which triggers
                    // CommunicationDeviceChangedListener → nativeOnAudioRouteChanged.
                    nativeOnDeviceConnected()
                } else {
                    onRouteChanged()
                }
            }

            override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
                if (removedDevices.isNullOrEmpty()) return
                Log.i(TAG, "AudioDeviceCallback: ${removedDevices.size} device(s) removed")

                if (!voiceModeEnabled) return

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    nativeOnDeviceDisconnected()
                } else {
                    onRouteChanged()
                }
            }
        }

        am.registerAudioDeviceCallback(callback, mainHandler)
        audioDeviceCallback = callback
        Log.d(TAG, "AudioDeviceCallback registered")
    }

    private fun unregisterAudioDeviceCallback() {
        val am = audioManager ?: return
        audioDeviceCallback?.let {
            am.unregisterAudioDeviceCallback(it)
        }
        audioDeviceCallback = null
    }


    /**
     * Route to the best available device (API 31+).
     * Prefers external devices (BT, wired, USB) over builtin speaker/earpiece.
     * When an external device is found, setCommunicationDevice triggers
     * CommunicationDeviceChangedListener → nativeOnAudioRouteChanged → JS notified.
     * Dedup logic is handled by the C++ RouteHandler.
     * Called from C++ via JNI on device connect/disconnect.
     */
    @JvmStatic
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    fun routeToBestDeviceNow() {
        val am = audioManager ?: return

        // Prefer wired/USB (deterministic, low latency) over BT.
        // When both BT SCO and A2DP are available, honor the user's last explicit
        // BT type preference (preferredBtType) instead of always picking SCO.
        val externalTypes = setOf(
            AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
            AudioDeviceInfo.TYPE_BLE_HEADSET,
            AudioDeviceInfo.TYPE_BLE_SPEAKER,
            AudioDeviceInfo.TYPE_WIRED_HEADSET,
            AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
            AudioDeviceInfo.TYPE_USB_DEVICE,
            AudioDeviceInfo.TYPE_USB_HEADSET
        )
        val candidates = am.availableCommunicationDevices.filter { it.type in externalTypes }
        val nonBt = candidates.filter { !isBtType(it.type) }
        // Wired/USB always wins (lowest latency), then user's preferred BT, then any BT.
        // Match BLE counterparts: BLE_HEADSET is equivalent to SCO, BLE_SPEAKER to A2DP.
        val externalDevice = nonBt.minByOrNull { deviceRoutePriority(it.type) }
            ?: candidates.firstOrNull { it.type == preferredBtType || isPreferredBleCounterpart(it.type) }
            ?: run {
                val fallback = candidates.minByOrNull { deviceRoutePriority(it.type) }
                if (fallback != null) {
                    Log.i(TAG, "routeToBestDevice: preferred BT type not available, falling back to type=${fallback.type}")
                }
                fallback
            }

        if (externalDevice != null) {
            val success = am.setCommunicationDevice(externalDevice)
            Log.i(TAG, "routeToBestDevice: type=${externalDevice.type} success=$success")
        } else {
            // No external device available — fall back to builtin based on user preference.
            // If a device connects later, AudioDeviceCallback will re-trigger routing.
            val currentDevice = am.communicationDevice
            val currentType = currentDevice?.type
            val targetType = if (speakerPreferred) {
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
            } else {
                AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
            }
            if (currentType != targetType) {
                val device = am.availableCommunicationDevices.firstOrNull { it.type == targetType }
                if (device != null) {
                    // setCommunicationDevice triggers CommunicationDeviceChangedListener
                    // → nativeOnAudioRouteChanged → JS gets route + device list.
                    am.setCommunicationDevice(device)
                }
                Log.i(TAG, "routeToBestDevice: switching to builtin (speaker=$speakerPreferred)")
            } else {
                // Already on the correct builtin — route hasn't changed, but the
                // available device list has (external device was added/removed).
                // The SM's handleStable() fires adapter_.fireJsCallback() after
                // routeToBestDevice() returns, so JS gets the updated device list.
                Log.i(TAG, "routeToBestDevice: already on preferred builtin (speaker=$speakerPreferred)")
            }
        }
    }

    @JvmStatic
    fun getCurrentRoute(): Int {
        val am = audioManager ?: return Route.UNKNOWN

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val device = am.communicationDevice
            return if (device != null) deviceTypeToRoute(device.type) else Route.SPEAKER
        }

        @Suppress("DEPRECATION")
        return when {
            am.isBluetoothScoOn -> Route.BLUETOOTH_SCO
            am.isBluetoothA2dpOn -> Route.BLUETOOTH_A2DP
            am.isWiredHeadsetOn -> Route.WIRED_HEADSET
            am.isSpeakerphoneOn -> Route.SPEAKER
            else -> Route.EARPIECE
        }
    }

    // Lower value = higher priority for auto-routing.
    // Wired/USB preferred (deterministic, low latency), then BT SCO (bidirectional),
    // then BT A2DP (output-only, higher latency).
    private fun deviceRoutePriority(type: Int): Int = when (type) {
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> 0
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET -> 1
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLE_HEADSET -> 2
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> 3
        else -> 99
    }

    private fun deviceTypeToRoute(type: Int): Int = when (type) {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLE_HEADSET -> Route.BLUETOOTH_SCO
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> Route.BLUETOOTH_A2DP
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> Route.WIRED_HEADSET
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET -> Route.USB_DEVICE
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> Route.SPEAKER
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> Route.EARPIECE
        else -> Route.UNKNOWN
    }

    @JvmStatic
    fun getEchoModeForRoute(route: Int): Int {
        return when (route) {
            // A2DP is output-only — the device mic stays active for capture,
            // so the platform's built-in AEC remains effective (parity with iOS).
            Route.SPEAKER, Route.EARPIECE, Route.BLUETOOTH_A2DP -> EchoMode.HARDWARE_AEC
            Route.BLUETOOTH_SCO -> EchoMode.BLUETOOTH_AEC
            Route.WIRED_HEADSET, Route.USB_DEVICE -> EchoMode.NONE
            else -> EchoMode.NONE
        }
    }

    @Suppress("MissingPermission")
    private fun resolveDeviceName(device: AudioDeviceInfo, route: Int): String {
        if (route == Route.BLUETOOTH_SCO || route == Route.BLUETOOTH_A2DP) {
            resolveBluetoothName(device)?.let { return it }
        }
        return device.productName?.toString()?.takeIf { it.isNotBlank() }
            ?: routeToString(route)
    }

    // AudioDeviceInfo.getProductName() returns Build.MODEL for BT devices (AOSP fallback
    // when the audio HAL doesn't populate the port name). Resolve the real headset name
    // via BluetoothAdapter APIs instead.
    @Suppress("MissingPermission")
    private fun resolveBluetoothName(device: AudioDeviceInfo): String? {
        val ctx = context ?: return null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) return null

        val adapter = BluetoothAdapter.getDefaultAdapter() ?: return null

        // Try MAC address directly from the AudioDeviceInfo
        nameFromAddress(adapter, device.address)?.let { return it }

        // Communication devices often have empty addresses. Output devices from
        // getDevices(GET_DEVICES_OUTPUTS) come from a different AudioPort path
        // and may have the address populated.
        val am = audioManager ?: return null
        val outputDevice = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull {
            it.type == device.type
        }
        if (outputDevice != null) {
            nameFromAddress(adapter, outputDevice.address)?.let { return it }
        }

        // Fall back to cached name from BT broadcast intent
        connectedBtDeviceName?.let { return it }

        // Last resort: match bonded devices by address against connected output devices
        val connectedAddresses = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            .filter { isBtType(it.type) }
            .mapNotNull { it.address?.takeIf { addr -> addr.isNotBlank() && BluetoothAdapter.checkBluetoothAddress(addr) } }
            .toSet()
        if (connectedAddresses.isNotEmpty()) {
            val match = adapter.bondedDevices?.firstOrNull { it.address in connectedAddresses }
            if (match != null) return btDeviceName(match)
        }

        return null
    }

    @Suppress("MissingPermission")
    private fun nameFromAddress(adapter: BluetoothAdapter, address: String?): String? {
        if (address.isNullOrBlank() || !BluetoothAdapter.checkBluetoothAddress(address)) return null
        return try {
            btDeviceName(adapter.getRemoteDevice(address))
        } catch (_: Exception) {
            null
        }
    }

    @Suppress("MissingPermission")
    private fun cacheBtDeviceName(intent: Intent, connected: Boolean) {
        if (!connected) {
            // Only clear cache if the disconnecting device matches the cached name.
            // Prevents clearing the cache when a different BT device disconnects
            // while our target device is still connected.
            val btDevice = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE, android.bluetooth.BluetoothDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE)
            }
            val disconnectingName = btDevice?.let { btDeviceName(it) }
            if (disconnectingName == null || disconnectingName == connectedBtDeviceName) {
                connectedBtDeviceName = null
            }
            return
        }
        val btDevice = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE, android.bluetooth.BluetoothDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE)
        }
        if (btDevice != null) {
            connectedBtDeviceName = btDeviceName(btDevice)
        }
    }

    @Suppress("MissingPermission")
    private fun queryConnectedBtDeviceName() {
        if (connectedBtDeviceName != null) return
        val ctx = context ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) return

        val adapter = BluetoothAdapter.getDefaultAdapter() ?: return
        adapter.getProfileProxy(ctx, object : BluetoothProfile.ServiceListener {
            @Suppress("MissingPermission")
            override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
                val devices = proxy.connectedDevices
                if (devices.isNotEmpty() && connectedBtDeviceName == null) {
                    connectedBtDeviceName = btDeviceName(devices[0])
                }
                adapter.closeProfileProxy(profile, proxy)
            }
            override fun onServiceDisconnected(profile: Int) {}
        }, BluetoothProfile.HEADSET)
    }

    @Suppress("MissingPermission")
    private fun btDeviceName(device: android.bluetooth.BluetoothDevice): String? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            device.alias ?: device.name
        } else {
            device.name
        }
    }

    private fun registerRouteReceiver() {
        val ctx = context ?: return

        routeReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                when (intent?.action) {
                    AudioManager.ACTION_HEADSET_PLUG -> {
                        val state = intent.getIntExtra("state", -1)
                        Log.i(TAG, "Headset plug: state=$state")
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            // setCommunicationDevice triggers CommunicationDeviceChangedListener
                            routeToBestDeviceNow()
                        } else {
                            onRouteChanged()
                        }
                    }
                    AudioManager.ACTION_AUDIO_BECOMING_NOISY -> {
                        // Pre-31: noisy transition needs explicit route detection.
                        // API 31+: ACTION_HEADSET_PLUG already handled via routeToBestDevice.
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                            onRouteChanged()
                        }
                    }
                }
            }
        }

        val filter = IntentFilter().apply {
            addAction(AudioManager.ACTION_AUDIO_BECOMING_NOISY)
            addAction(AudioManager.ACTION_HEADSET_PLUG)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            ctx.registerReceiver(routeReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            ctx.registerReceiver(routeReceiver, filter)
        }
        Log.d(TAG, "Route receiver registered")
    }

    private fun unregisterRouteReceiver() {
        val ctx = context ?: return
        routeReceiver?.let {
            try {
                ctx.unregisterReceiver(it)
            } catch (e: Exception) {
                Log.w(TAG, "Error unregistering receiver: ${e.message}")
            }
        }
        routeReceiver = null
    }

    private fun onRouteChanged() {
        val route = getCurrentRoute()
        val echoMode = getEchoModeForRoute(route)
        val deviceId = getCurrentRouteDeviceId()

        // Dedup and restart decisions are handled by the C++ RouteHandler.
        Log.i(TAG, "Route changed: ${routeToString(route)}, echo: ${echoModeToString(echoMode)}")
        nativeOnAudioRouteChanged(route, echoMode, deviceId)
    }

    private fun getCurrentRouteDeviceId(): String {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val am = audioManager ?: return ""
            return am.communicationDevice?.id?.toString() ?: ""
        }
        return ""
    }

    private fun routeToString(route: Int): String = when (route) {
        Route.EARPIECE -> "Earpiece"
        Route.SPEAKER -> "Speaker"
        Route.WIRED_HEADSET -> "WiredHeadset"
        Route.BLUETOOTH_SCO -> "BluetoothSCO"
        Route.BLUETOOTH_A2DP -> "BluetoothA2DP"
        Route.USB_DEVICE -> "USB"
        else -> "Unknown"
    }

    private fun echoModeToString(mode: Int): String = when (mode) {
        EchoMode.HARDWARE_AEC -> "HardwareAEC"
        EchoMode.BLUETOOTH_AEC -> "BluetoothAEC"
        EchoMode.NONE -> "None"
        else -> "Unknown"
    }

    private fun focusStateToString(state: Int): String = when (state) {
        FocusState.NONE -> "None"
        FocusState.GAIN -> "Gain"
        FocusState.LOSS -> "Loss"
        FocusState.LOSS_TRANSIENT -> "LossTransient"
        FocusState.LOSS_TRANSIENT_CAN_DUCK -> "LossTransientCanDuck"
        else -> "Unknown"
    }

    private fun scoAudioStateToString(state: Int): String = when (state) {
        AudioManager.SCO_AUDIO_STATE_CONNECTED -> "Connected"
        AudioManager.SCO_AUDIO_STATE_DISCONNECTED -> "Disconnected"
        AudioManager.SCO_AUDIO_STATE_CONNECTING -> "Connecting"
        AudioManager.SCO_AUDIO_STATE_ERROR -> "Error"
        else -> "Unknown($state)"
    }

    /**
     * Returns true if AudioManager.communicationDevice is non-null.
     * Called from C++ SamsungNullDeviceQuirk to verify spurious null events.
     */
    @JvmStatic
    fun validateDeviceState(): Boolean {
        val am = audioManager ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            am.communicationDevice != null
        } else {
            false
        }
    }

    /**
     * Re-assert MODE_IN_COMMUNICATION if it was reset (Xiaomi MIUI bug).
     * Called from C++ XiaomiModeResetQuirk on LifecycleForeground.
     */
    @JvmStatic
    fun reassertVoiceMode() {
        val am = audioManager ?: return
        if (voiceModeEnabled && activeStreams.get() > 0) {
            if (am.mode != AudioManager.MODE_IN_COMMUNICATION) {
                Log.w(TAG, "reassertVoiceMode: mode was reset (mode=${am.mode}), re-asserting")
                am.mode = AudioManager.MODE_IN_COMMUNICATION
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    routeToBestDeviceNow()
                }
            }
        }
    }

    private external fun nativeInit()
    private external fun nativeDestroy()
    private external fun nativeOnSessionIdGenerated(sessionId: Int)
    private external fun nativeSetVoiceMode(enable: Boolean)
    private external fun nativeOnAudioRouteChanged(route: Int, echoMode: Int, deviceId: String)
    private external fun nativeOnAudioFocusChanged(focusState: Int)
    private external fun nativeOnLifecycleChanged(foreground: Boolean)
    private external fun nativeOnDeviceConnected()
    private external fun nativeOnDeviceDisconnected()
}

// AudioSessionBridge.kt - Manages audio routing and Bluetooth for playback
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
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner

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

    @Volatile private var audioManager: AudioManager? = null
    @Volatile private var context: Context? = null
    @Volatile private var initialized = false
    @Volatile private var speakerPreferred = true  // Default to speaker on app start
    @Volatile private var hasEarpiece = true  // Cached at init, false on tablets
    @Volatile private var connectedBtDeviceName: String? = null
    @Volatile private var preferredBtType: Int = AudioDeviceInfo.TYPE_BLUETOOTH_SCO
    @Volatile private var audioFocusRequest: AudioFocusRequest? = null
    private val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())

    private var routeReceiver: BroadcastReceiver? = null
    private var bluetoothReceiver: BroadcastReceiver? = null
    private var communicationDeviceListener: Any? = null
    private var audioDeviceCallback: AudioDeviceCallback? = null

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
    fun setAudioRoute(route: Int, deviceId: String = ""): Boolean {
        val am = audioManager ?: return false

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
                am.startBluetoothSco()
                am.isBluetoothScoOn = true
                Log.i(TAG, "setAudioRoute (legacy): BT SCO requested")
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
        // same thread (non-recursive mutex -> deadlock). Posting to mainHandler defers the
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
     * Encoding: for each device, emit [route, deviceName, deviceId] as String[].
     */
    @JvmStatic
    fun getAvailableAudioDevices(): Array<String> {
        val am = audioManager ?: return emptyArray()
        val result = mutableListOf<String>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
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
                            routeToBestDeviceNow()
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
                            routeToBestDeviceNow()
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

                        // Pre-31: need explicit route detection for SCO audio state changes.
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                            onRouteChanged()
                        }
                    }
                    BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED -> {
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
     * notifications on API 31+.
     */
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun registerCommunicationDeviceListener() {
        val am = audioManager ?: return

        val listener = AudioManager.OnCommunicationDeviceChangedListener { device ->
            // Samsung One UI 5.x+ sends spurious null during internal audio policy
            // reconfiguration. Verify by checking the actual communication device state --
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
            Log.i(TAG, "CommunicationDevice changed: ${routeToString(route)} (type=${device?.type})")

            val deviceId = device?.id?.toString() ?: ""

            nativeOnAudioRouteChanged(route, deviceId)
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
     */
    private fun registerAudioDeviceCallback() {
        val am = audioManager ?: return

        val callback = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
                if (addedDevices.isNullOrEmpty()) return
                Log.i(TAG, "AudioDeviceCallback: ${addedDevices.size} device(s) added")

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    routeToBestDeviceNow()
                } else {
                    onRouteChanged()
                }
            }

            override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
                if (removedDevices.isNullOrEmpty()) return
                Log.i(TAG, "AudioDeviceCallback: ${removedDevices.size} device(s) removed")

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    routeToBestDeviceNow()
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
     */
    @JvmStatic
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    fun routeToBestDeviceNow() {
        val am = audioManager ?: return

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
            // No external device available -- fall back to builtin based on user preference.
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
                    am.setCommunicationDevice(device)
                }
                Log.i(TAG, "routeToBestDevice: switching to builtin (speaker=$speakerPreferred)")
            } else {
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
    private fun btDeviceName(device: android.bluetooth.BluetoothDevice): String? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            device.alias ?: device.name
        } else {
            device.name
        }
    }

    private fun requestAudioFocus() {
        val am = audioManager ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                .build()
            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attrs)
                .setOnAudioFocusChangeListener { focusChange ->
                    Log.i(TAG, "AudioFocus changed: $focusChange")
                    nativeOnAudioFocusChanged(focusChange)
                }
                .build()
            val result = am.requestAudioFocus(request)
            if (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
                audioFocusRequest = request
                Log.i(TAG, "Audio focus granted")
            } else {
                Log.w(TAG, "Audio focus request failed: $result")
            }
        }
    }

    private fun abandonAudioFocus() {
        val am = audioManager ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            audioFocusRequest?.let {
                am.abandonAudioFocusRequest(it)
                audioFocusRequest = null
                Log.i(TAG, "Audio focus abandoned")
            }
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
                            routeToBestDeviceNow()
                        } else {
                            onRouteChanged()
                        }
                    }
                    AudioManager.ACTION_AUDIO_BECOMING_NOISY -> {
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
        val deviceId = getCurrentRouteDeviceId()

        Log.i(TAG, "Route changed: ${routeToString(route)}")
        nativeOnAudioRouteChanged(route, deviceId)
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

    private fun scoAudioStateToString(state: Int): String = when (state) {
        AudioManager.SCO_AUDIO_STATE_CONNECTED -> "Connected"
        AudioManager.SCO_AUDIO_STATE_DISCONNECTED -> "Disconnected"
        AudioManager.SCO_AUDIO_STATE_CONNECTING -> "Connecting"
        AudioManager.SCO_AUDIO_STATE_ERROR -> "Error"
        else -> "Unknown($state)"
    }

    private external fun nativeInit()
    private external fun nativeDestroy()
    private external fun nativeOnAudioRouteChanged(route: Int, deviceId: String)
    private external fun nativeOnAudioFocusChanged(focusChange: Int)
}

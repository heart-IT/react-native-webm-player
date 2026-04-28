// Route-setting paths: API-31+ communication-device flow, pre-API-31 legacy
// flow, and the auto-pick-best-external-device (`routeToBestDeviceNow`).
//
// Reads/writes AudioSessionBridge state directly (audioManager,
// speakerPreferred, preferredBtType, hasEarpiece). The public
// AudioSessionBridge.setAudioRoute() is a thin delegate to the routines here.
package com.heartit.webmplayer.audio

import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.util.Log

internal object AudioRouteSetter {

    fun setRoute(route: Int, deviceId: String): Boolean {
        val am = AudioSessionBridge.audioManager ?: return false

        Log.i(AudioSessionBridge.TAG,
            "setAudioRoute: ${AudioRoutePriority.routeToString(route)}" +
                if (deviceId.isNotEmpty()) " deviceId=$deviceId" else "")

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            setRouteModern(am, route, deviceId)
        } else {
            setRoutePreApi31(am, route)
        }
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun setRouteModern(am: AudioManager, route: Int, deviceId: String): Boolean {
        val targetId = if (deviceId.isNotEmpty()) deviceId.toIntOrNull() else null

        val device = when (route) {
            AudioSessionBridge.Route.SPEAKER -> am.availableCommunicationDevices.firstOrNull {
                it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER &&
                    (targetId == null || it.id == targetId)
            }
            AudioSessionBridge.Route.EARPIECE -> am.availableCommunicationDevices.firstOrNull {
                it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE &&
                    (targetId == null || it.id == targetId)
            }
            AudioSessionBridge.Route.BLUETOOTH_SCO -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
                    it.type == AudioDeviceInfo.TYPE_BLE_HEADSET) &&
                    (targetId == null || it.id == targetId)
            }
            AudioSessionBridge.Route.BLUETOOTH_A2DP -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                    it.type == AudioDeviceInfo.TYPE_BLE_SPEAKER) &&
                    (targetId == null || it.id == targetId)
            }
            AudioSessionBridge.Route.WIRED_HEADSET -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                    it.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES) &&
                    (targetId == null || it.id == targetId)
            }
            AudioSessionBridge.Route.USB_DEVICE -> am.availableCommunicationDevices.firstOrNull {
                (it.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                    it.type == AudioDeviceInfo.TYPE_USB_HEADSET) &&
                    (targetId == null || it.id == targetId)
            }
            else -> null
        }

        if (device == null) {
            Log.w(AudioSessionBridge.TAG,
                "setAudioRoute: device for ${AudioRoutePriority.routeToString(route)} not available")
            return false
        }

        // Update state BEFORE setCommunicationDevice() so the
        // CommunicationDeviceChangedListener reads correct values when it fires.
        // Save previous values for rollback if setCommunicationDevice() fails.
        val prevSpeakerPreferred = AudioSessionBridge.speakerPreferred
        val prevPreferredBtType = AudioSessionBridge.preferredBtType
        if (route == AudioSessionBridge.Route.SPEAKER || route == AudioSessionBridge.Route.EARPIECE) {
            AudioSessionBridge.speakerPreferred = (route == AudioSessionBridge.Route.SPEAKER)
        }
        if (route == AudioSessionBridge.Route.BLUETOOTH_SCO) {
            AudioSessionBridge.preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_SCO
        } else if (route == AudioSessionBridge.Route.BLUETOOTH_A2DP) {
            AudioSessionBridge.preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
        }
        val success = am.setCommunicationDevice(device)
        if (success) {
            Log.i(AudioSessionBridge.TAG,
                "setAudioRoute: set to ${AudioRoutePriority.routeToString(route)}")
            // CommunicationDeviceChangedListener handles nativeOnAudioRouteChanged
        } else {
            Log.w(AudioSessionBridge.TAG, "setAudioRoute: setCommunicationDevice failed, rolling back state")
            AudioSessionBridge.speakerPreferred = prevSpeakerPreferred
            AudioSessionBridge.preferredBtType = prevPreferredBtType
        }
        return success
    }

    @Suppress("DEPRECATION")
    private fun setRoutePreApi31(am: AudioManager, route: Int): Boolean {
        when (route) {
            AudioSessionBridge.Route.SPEAKER -> {
                am.isSpeakerphoneOn = true
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                AudioSessionBridge.speakerPreferred = true
            }
            AudioSessionBridge.Route.EARPIECE -> {
                if (!AudioSessionBridge.hasEarpiece) {
                    Log.w(AudioSessionBridge.TAG,
                        "setAudioRoute (pre-API-31): earpiece not available on this device")
                    return false
                }
                am.isSpeakerphoneOn = false
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                AudioSessionBridge.speakerPreferred = false
            }
            AudioSessionBridge.Route.BLUETOOTH_SCO -> {
                am.isSpeakerphoneOn = false
                am.startBluetoothSco()
                am.isBluetoothScoOn = true
                Log.i(AudioSessionBridge.TAG, "setAudioRoute (pre-API-31): BT SCO requested")
                return true
            }
            AudioSessionBridge.Route.BLUETOOTH_A2DP -> {
                // Pre-API 31: no direct A2DP routing. Clear SCO and speakerphone
                // so the system routes to the connected A2DP device by default.
                if (!hasConnectedBluetoothA2dpDevice(am)) {
                    Log.w(AudioSessionBridge.TAG, "setAudioRoute (pre-API-31): no A2DP device connected")
                    return false
                }
                am.isSpeakerphoneOn = false
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                AudioSessionBridge.speakerPreferred = false
                AudioSessionBridge.preferredBtType = AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
            }
            AudioSessionBridge.Route.WIRED_HEADSET, AudioSessionBridge.Route.USB_DEVICE -> {
                // Pre-API 31: no setCommunicationDevice(). Clear SCO and speakerphone
                // so the system routes to the connected wired/USB device by default.
                if (!am.isWiredHeadsetOn && !hasConnectedUsbAudioDevice(am)) {
                    Log.w(AudioSessionBridge.TAG, "setAudioRoute (pre-API-31): no wired/USB device connected")
                    return false
                }
                am.isSpeakerphoneOn = false
                am.stopBluetoothSco()
                am.isBluetoothScoOn = false
                AudioSessionBridge.speakerPreferred = false
            }
            else -> return false
        }

        Log.i(AudioSessionBridge.TAG,
            "setAudioRoute (pre-API-31): set to ${AudioRoutePriority.routeToString(route)}")
        // Post route notification asynchronously. On pre-API 31, setAudioRoute() may be
        // called from native via JNI while the C++ routingMutex_ is held. Calling
        // nativeOnAudioRouteChanged synchronously would re-acquire routingMutex_ on the
        // same thread (non-recursive mutex -> deadlock). Posting to mainHandler defers the
        // notification until after the JNI call returns and routingMutex_ is released.
        AudioSessionBridge.mainHandler.post { AudioSessionBridge.onRouteChanged() }
        return true
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    fun routeToBestDeviceNow() {
        val am = AudioSessionBridge.audioManager ?: return

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
        val nonBt = candidates.filter { !AudioRoutePriority.isBtType(it.type) }
        // Wired/USB always wins (lowest latency), then user's preferred BT, then any BT.
        val externalDevice = nonBt.minByOrNull { AudioRoutePriority.deviceRoutePriority(it.type) }
            ?: candidates.firstOrNull {
                it.type == AudioSessionBridge.preferredBtType ||
                    isPreferredBleCounterpart(it.type)
            }
            ?: run {
                val fallback = candidates.minByOrNull { AudioRoutePriority.deviceRoutePriority(it.type) }
                if (fallback != null) {
                    Log.i(AudioSessionBridge.TAG,
                        "routeToBestDevice: preferred BT type not available, falling back to type=${fallback.type}")
                }
                fallback
            }

        if (externalDevice != null) {
            val success = am.setCommunicationDevice(externalDevice)
            Log.i(AudioSessionBridge.TAG,
                "routeToBestDevice: type=${externalDevice.type} success=$success")
        } else {
            // No external device available -- fall back to builtin based on user preference.
            val currentDevice = am.communicationDevice
            val currentType = currentDevice?.type
            val targetType = if (AudioSessionBridge.speakerPreferred) {
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
            } else {
                AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
            }
            if (currentType != targetType) {
                val device = am.availableCommunicationDevices.firstOrNull { it.type == targetType }
                if (device != null) {
                    am.setCommunicationDevice(device)
                }
                Log.i(AudioSessionBridge.TAG,
                    "routeToBestDevice: switching to builtin (speaker=${AudioSessionBridge.speakerPreferred})")
            } else {
                Log.i(AudioSessionBridge.TAG,
                    "routeToBestDevice: already on preferred builtin (speaker=${AudioSessionBridge.speakerPreferred})")
            }
        }
    }

    fun hasConnectedUsbAudioDevice(am: AudioManager): Boolean {
        return am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
        }
    }

    fun hasConnectedBluetoothScoDevice(am: AudioManager): Boolean {
        return am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
        }
    }

    fun hasConnectedBluetoothA2dpDevice(am: AudioManager): Boolean {
        return am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
        }
    }

    // BLE_HEADSET is equivalent to SCO (bidirectional), BLE_SPEAKER to A2DP (output-only).
    private fun isPreferredBleCounterpart(type: Int): Boolean = when (AudioSessionBridge.preferredBtType) {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> type == AudioDeviceInfo.TYPE_BLE_HEADSET
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> type == AudioDeviceInfo.TYPE_BLE_SPEAKER
        else -> false
    }
}

// Pure mappings between Android AudioDeviceInfo types, our internal Route
// enum, and human-readable labels. Stateless — every input maps to a single
// output by construction. Lifted out of AudioSessionBridge so that file can
// focus on lifecycle + state.
package com.heartit.webmplayer.audio

import android.media.AudioDeviceInfo
import android.media.AudioManager

internal object AudioRoutePriority {
    // Lower value = higher priority for auto-routing.
    fun deviceRoutePriority(type: Int): Int = when (type) {
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

    fun deviceTypeToRoute(type: Int): Int = when (type) {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLE_HEADSET -> AudioSessionBridge.Route.BLUETOOTH_SCO
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> AudioSessionBridge.Route.BLUETOOTH_A2DP
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> AudioSessionBridge.Route.WIRED_HEADSET
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET -> AudioSessionBridge.Route.USB_DEVICE
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> AudioSessionBridge.Route.SPEAKER
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> AudioSessionBridge.Route.EARPIECE
        else -> AudioSessionBridge.Route.UNKNOWN
    }

    fun isBtType(type: Int): Boolean = when (type) {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_HEADSET,
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> true
        else -> false
    }

    fun routeToString(route: Int): String = when (route) {
        AudioSessionBridge.Route.EARPIECE -> "Earpiece"
        AudioSessionBridge.Route.SPEAKER -> "Speaker"
        AudioSessionBridge.Route.WIRED_HEADSET -> "WiredHeadset"
        AudioSessionBridge.Route.BLUETOOTH_SCO -> "BluetoothSCO"
        AudioSessionBridge.Route.BLUETOOTH_A2DP -> "BluetoothA2DP"
        AudioSessionBridge.Route.USB_DEVICE -> "USB"
        else -> "Unknown"
    }

    fun scoAudioStateToString(state: Int): String = when (state) {
        AudioManager.SCO_AUDIO_STATE_CONNECTED -> "Connected"
        AudioManager.SCO_AUDIO_STATE_DISCONNECTED -> "Disconnected"
        AudioManager.SCO_AUDIO_STATE_CONNECTING -> "Connecting"
        AudioManager.SCO_AUDIO_STATE_ERROR -> "Error"
        else -> "Unknown($state)"
    }
}

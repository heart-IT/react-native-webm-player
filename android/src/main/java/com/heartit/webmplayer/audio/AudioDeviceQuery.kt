// Read-only audio device queries: enumerate available routes, enumerate
// every individual device with name + ID, resolve the currently-active
// route, and look up human-readable names for Bluetooth devices.
//
// All paths read state on AudioSessionBridge (audioManager, hasEarpiece,
// connectedBtDeviceName, context) but never mutate it.
package com.heartit.webmplayer.audio

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.pm.PackageManager
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import androidx.core.content.ContextCompat

internal object AudioDeviceQuery {

    fun getAvailableAudioRoutes(): IntArray {
        val am = AudioSessionBridge.audioManager ?: return intArrayOf()
        val routes = mutableListOf<Int>()

        // Earpiece only on phones (parity with iOS UIUserInterfaceIdiomPhone check)
        if (AudioSessionBridge.hasEarpiece) routes.add(AudioSessionBridge.Route.EARPIECE)
        routes.add(AudioSessionBridge.Route.SPEAKER)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            for (device in am.availableCommunicationDevices) {
                when (device.type) {
                    AudioDeviceInfo.TYPE_WIRED_HEADSET,
                    AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> {
                        if (!routes.contains(AudioSessionBridge.Route.WIRED_HEADSET))
                            routes.add(AudioSessionBridge.Route.WIRED_HEADSET)
                    }
                    AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> {
                        if (!routes.contains(AudioSessionBridge.Route.BLUETOOTH_SCO))
                            routes.add(AudioSessionBridge.Route.BLUETOOTH_SCO)
                    }
                    AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> {
                        if (!routes.contains(AudioSessionBridge.Route.BLUETOOTH_A2DP))
                            routes.add(AudioSessionBridge.Route.BLUETOOTH_A2DP)
                    }
                    AudioDeviceInfo.TYPE_USB_DEVICE,
                    AudioDeviceInfo.TYPE_USB_HEADSET -> {
                        if (!routes.contains(AudioSessionBridge.Route.USB_DEVICE))
                            routes.add(AudioSessionBridge.Route.USB_DEVICE)
                    }
                    AudioDeviceInfo.TYPE_BLE_HEADSET -> {
                        if (!routes.contains(AudioSessionBridge.Route.BLUETOOTH_SCO))
                            routes.add(AudioSessionBridge.Route.BLUETOOTH_SCO)
                    }
                    AudioDeviceInfo.TYPE_BLE_SPEAKER -> {
                        if (!routes.contains(AudioSessionBridge.Route.BLUETOOTH_A2DP))
                            routes.add(AudioSessionBridge.Route.BLUETOOTH_A2DP)
                    }
                }
            }
        } else {
            @Suppress("DEPRECATION")
            if (am.isWiredHeadsetOn) routes.add(AudioSessionBridge.Route.WIRED_HEADSET)
            if (AudioRouteSetter.hasConnectedBluetoothScoDevice(am))
                routes.add(AudioSessionBridge.Route.BLUETOOTH_SCO)
            if (AudioRouteSetter.hasConnectedBluetoothA2dpDevice(am))
                routes.add(AudioSessionBridge.Route.BLUETOOTH_A2DP)
            if (AudioRouteSetter.hasConnectedUsbAudioDevice(am))
                routes.add(AudioSessionBridge.Route.USB_DEVICE)
        }

        return routes.toIntArray()
    }

    fun getAvailableAudioDevices(): Array<String> {
        val am = AudioSessionBridge.audioManager ?: return emptyArray()
        val result = mutableListOf<String>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val seen = mutableSetOf<Int>()

            // Earpiece first (parity with iOS ordering)
            if (AudioSessionBridge.hasEarpiece) {
                for (device in am.availableCommunicationDevices) {
                    if (device.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE && device.id !in seen) {
                        seen.add(device.id)
                        result.add(AudioSessionBridge.Route.EARPIECE.toString())
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
                    result.add(AudioSessionBridge.Route.SPEAKER.toString())
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
                    AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> AudioSessionBridge.Route.WIRED_HEADSET
                    AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
                    AudioDeviceInfo.TYPE_BLE_HEADSET -> AudioSessionBridge.Route.BLUETOOTH_SCO
                    AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                    AudioDeviceInfo.TYPE_BLE_SPEAKER -> AudioSessionBridge.Route.BLUETOOTH_A2DP
                    AudioDeviceInfo.TYPE_USB_DEVICE,
                    AudioDeviceInfo.TYPE_USB_HEADSET -> AudioSessionBridge.Route.USB_DEVICE
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
            if (AudioSessionBridge.hasEarpiece) {
                result.add(AudioSessionBridge.Route.EARPIECE.toString())
                result.add("Earpiece")
                result.add("builtin-earpiece")
            }

            result.add(AudioSessionBridge.Route.SPEAKER.toString())
            result.add("Speaker")
            result.add("builtin-speaker")

            @Suppress("DEPRECATION")
            if (am.isWiredHeadsetOn) {
                result.add(AudioSessionBridge.Route.WIRED_HEADSET.toString())
                result.add("Wired Headset")
                result.add("wired-headset")
            }
            if (AudioRouteSetter.hasConnectedBluetoothScoDevice(am)) {
                result.add(AudioSessionBridge.Route.BLUETOOTH_SCO.toString())
                result.add(AudioSessionBridge.connectedBtDeviceName ?: "Bluetooth SCO")
                result.add("bluetooth-sco")
            }
            if (AudioRouteSetter.hasConnectedBluetoothA2dpDevice(am)) {
                result.add(AudioSessionBridge.Route.BLUETOOTH_A2DP.toString())
                result.add(AudioSessionBridge.connectedBtDeviceName ?: "Bluetooth A2DP")
                result.add("bluetooth-a2dp")
            }
            if (AudioRouteSetter.hasConnectedUsbAudioDevice(am)) {
                result.add(AudioSessionBridge.Route.USB_DEVICE.toString())
                result.add("USB Device")
                result.add("usb-device")
            }
        }

        return result.toTypedArray()
    }

    fun getCurrentRoute(): Int {
        val am = AudioSessionBridge.audioManager ?: return AudioSessionBridge.Route.UNKNOWN

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val device = am.communicationDevice
            return if (device != null) AudioRoutePriority.deviceTypeToRoute(device.type)
            else AudioSessionBridge.Route.SPEAKER
        }

        @Suppress("DEPRECATION")
        return when {
            am.isBluetoothScoOn -> AudioSessionBridge.Route.BLUETOOTH_SCO
            am.isBluetoothA2dpOn -> AudioSessionBridge.Route.BLUETOOTH_A2DP
            am.isWiredHeadsetOn -> AudioSessionBridge.Route.WIRED_HEADSET
            am.isSpeakerphoneOn -> AudioSessionBridge.Route.SPEAKER
            else -> AudioSessionBridge.Route.EARPIECE
        }
    }

    fun getCurrentRouteDeviceId(): String {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val am = AudioSessionBridge.audioManager ?: return ""
            return am.communicationDevice?.id?.toString() ?: ""
        }
        // Pre-API-31: emit the same synthetic IDs that getAvailableAudioDevices() reports
        // for each route, so RouteHandler dedup correctly distinguishes one route from
        // another. Returning "" would make every pre-API-31 route change look like the
        // same device, suppressing the stream restart that BT / wired / SCO transitions need.
        return when (getCurrentRoute()) {
            AudioSessionBridge.Route.EARPIECE -> "builtin-earpiece"
            AudioSessionBridge.Route.SPEAKER -> "builtin-speaker"
            AudioSessionBridge.Route.WIRED_HEADSET -> "wired-headset"
            AudioSessionBridge.Route.BLUETOOTH_SCO -> "bluetooth-sco"
            AudioSessionBridge.Route.BLUETOOTH_A2DP -> "bluetooth-a2dp"
            AudioSessionBridge.Route.USB_DEVICE -> "usb-device"
            else -> ""
        }
    }

    @Suppress("MissingPermission")
    private fun resolveDeviceName(device: AudioDeviceInfo, route: Int): String {
        if (route == AudioSessionBridge.Route.BLUETOOTH_SCO ||
            route == AudioSessionBridge.Route.BLUETOOTH_A2DP) {
            resolveBluetoothName(device)?.let { return it }
        }
        return device.productName?.toString()?.takeIf { it.isNotBlank() }
            ?: AudioRoutePriority.routeToString(route)
    }

    // AudioDeviceInfo.getProductName() returns Build.MODEL for BT devices (AOSP fallback
    // when the audio HAL doesn't populate the port name). Resolve the real headset name
    // via BluetoothAdapter APIs instead.
    @Suppress("MissingPermission")
    private fun resolveBluetoothName(device: AudioDeviceInfo): String? {
        val ctx = AudioSessionBridge.context ?: return null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) return null

        val adapter = BluetoothAdapter.getDefaultAdapter() ?: return null

        // Try MAC address directly from the AudioDeviceInfo
        nameFromAddress(adapter, device.address)?.let { return it }

        // Communication devices often have empty addresses. Output devices from
        // getDevices(GET_DEVICES_OUTPUTS) come from a different AudioPort path
        // and may have the address populated.
        val am = AudioSessionBridge.audioManager ?: return null
        val outputDevice = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull {
            it.type == device.type
        }
        if (outputDevice != null) {
            nameFromAddress(adapter, outputDevice.address)?.let { return it }
        }

        // Fall back to cached name from BT broadcast intent
        AudioSessionBridge.connectedBtDeviceName?.let { return it }

        // Last resort: match bonded devices by address against connected output devices
        val connectedAddresses = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            .filter { AudioRoutePriority.isBtType(it.type) }
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
    internal fun btDeviceName(device: android.bluetooth.BluetoothDevice): String? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            device.alias ?: device.name
        } else {
            device.name
        }
    }
}

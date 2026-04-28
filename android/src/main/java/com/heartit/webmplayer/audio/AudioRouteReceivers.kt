// Broadcast receivers + AudioManager listeners that detect audio device
// changes (BT connect/disconnect, wired headset plug, AudioDeviceCallback,
// API-31+ communication-device listener). Each register/unregister pair is
// a thin wrapper around the platform API; the actual reaction (re-routing,
// firing native callbacks) delegates back to AudioSessionBridge / AudioRouteSetter.
//
// Uses AudioSessionBridge.{routeReceiver, bluetoothReceiver,
// communicationDeviceListener, audioDeviceCallback} as the lifetime owners
// of the registered observers.
package com.heartit.webmplayer.audio

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
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat

internal object AudioRouteReceivers {

    fun registerBluetoothReceiver() {
        val ctx = AudioSessionBridge.context ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) {
            Log.w(AudioSessionBridge.TAG, "BLUETOOTH_CONNECT permission not granted, skipping Bluetooth receiver")
            return
        }

        val rcv = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                when (intent?.action) {
                    BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED -> {
                        val state = intent.getIntExtra(
                            BluetoothProfile.EXTRA_STATE,
                            BluetoothProfile.STATE_DISCONNECTED
                        )
                        val connected = state == BluetoothProfile.STATE_CONNECTED
                        cacheBtDeviceName(intent, connected)
                        Log.i(AudioSessionBridge.TAG,
                            "Bluetooth A2DP: ${if (connected) "connected" else "disconnected"}")
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            AudioRouteSetter.routeToBestDeviceNow()
                        } else if (connected) {
                            AudioSessionBridge.onRouteChanged()
                        } else {
                            val fallbackRoute = if (AudioSessionBridge.speakerPreferred)
                                AudioSessionBridge.Route.SPEAKER else AudioSessionBridge.Route.EARPIECE
                            AudioSessionBridge.setAudioRoute(fallbackRoute)
                        }
                    }
                    BluetoothHeadset.ACTION_CONNECTION_STATE_CHANGED -> {
                        val state = intent.getIntExtra(
                            BluetoothProfile.EXTRA_STATE,
                            BluetoothProfile.STATE_DISCONNECTED
                        )
                        val connected = state == BluetoothProfile.STATE_CONNECTED
                        cacheBtDeviceName(intent, connected)
                        Log.i(AudioSessionBridge.TAG,
                            "Bluetooth SCO headset: ${if (connected) "connected" else "disconnected"}")
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            AudioRouteSetter.routeToBestDeviceNow()
                        } else if (connected) {
                            AudioSessionBridge.onRouteChanged()
                        } else {
                            val fallbackRoute = if (AudioSessionBridge.speakerPreferred)
                                AudioSessionBridge.Route.SPEAKER else AudioSessionBridge.Route.EARPIECE
                            AudioSessionBridge.setAudioRoute(fallbackRoute)
                        }
                    }
                    AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED -> {
                        val scoState = intent.getIntExtra(
                            AudioManager.EXTRA_SCO_AUDIO_STATE,
                            AudioManager.SCO_AUDIO_STATE_DISCONNECTED
                        )
                        Log.i(AudioSessionBridge.TAG,
                            "Bluetooth SCO audio: ${AudioRoutePriority.scoAudioStateToString(scoState)}")

                        // Pre-31: need explicit route detection for SCO audio state changes.
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                            AudioSessionBridge.onRouteChanged()
                        }
                    }
                    BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED -> {
                        // Pre-31: may indicate BT availability change.
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                            AudioSessionBridge.onRouteChanged()
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
            ctx.registerReceiver(rcv, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            ctx.registerReceiver(rcv, filter)
        }
        AudioSessionBridge.bluetoothReceiver = rcv
        Log.d(AudioSessionBridge.TAG, "Bluetooth receiver registered")
    }

    fun unregisterBluetoothReceiver() {
        val ctx = AudioSessionBridge.context ?: return
        AudioSessionBridge.bluetoothReceiver?.let {
            try {
                ctx.unregisterReceiver(it)
            } catch (e: Exception) {
                Log.w(AudioSessionBridge.TAG, "Error unregistering Bluetooth receiver: ${e.message}")
            }
        }
        AudioSessionBridge.bluetoothReceiver = null
    }

    /**
     * Register listener for communication device changes (API 31+).
     * This listener fires when setCommunicationDevice() takes effect, receiving
     * the actual AudioDeviceInfo. It is the single source of truth for route
     * notifications on API 31+.
     */
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    fun registerCommunicationDeviceListener() {
        val am = AudioSessionBridge.audioManager ?: return

        val listener = AudioManager.OnCommunicationDeviceChangedListener { device ->
            // Samsung One UI 5.x+ sends spurious null during internal audio policy
            // reconfiguration. Verify by checking the actual communication device state --
            // if it's still set, the null is spurious and should be ignored.
            if (device == null) {
                val actual = am.communicationDevice
                if (actual != null) {
                    Log.i(AudioSessionBridge.TAG,
                        "CommunicationDevice: spurious null (actual type=${actual.type}), ignoring")
                    return@OnCommunicationDeviceChangedListener
                }
            }

            val route = if (device != null) AudioRoutePriority.deviceTypeToRoute(device.type)
                else if (AudioSessionBridge.speakerPreferred) AudioSessionBridge.Route.SPEAKER
                else AudioSessionBridge.Route.EARPIECE
            Log.i(AudioSessionBridge.TAG,
                "CommunicationDevice changed: ${AudioRoutePriority.routeToString(route)} (type=${device?.type})")

            val deviceId = device?.id?.toString() ?: ""
            AudioSessionBridge.fireNativeRouteChanged(route, deviceId)
        }
        am.addOnCommunicationDeviceChangedListener(AudioSessionBridge.mainHandler::post, listener)
        AudioSessionBridge.communicationDeviceListener = listener
        Log.d(AudioSessionBridge.TAG, "CommunicationDevice listener registered")
    }

    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    fun unregisterCommunicationDeviceListener() {
        val am = AudioSessionBridge.audioManager ?: return
        val listener = AudioSessionBridge.communicationDeviceListener
            as? AudioManager.OnCommunicationDeviceChangedListener
        if (listener != null) {
            am.removeOnCommunicationDeviceChangedListener(listener)
        }
        AudioSessionBridge.communicationDeviceListener = null
    }

    /**
     * Register AudioDeviceCallback for device availability changes.
     * Fires onAudioDevicesAdded/Removed when devices connect or disconnect from the system.
     */
    fun registerAudioDeviceCallback() {
        val am = AudioSessionBridge.audioManager ?: return

        val callback = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
                if (addedDevices.isNullOrEmpty()) return
                Log.i(AudioSessionBridge.TAG,
                    "AudioDeviceCallback: ${addedDevices.size} device(s) added")

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    AudioRouteSetter.routeToBestDeviceNow()
                } else {
                    AudioSessionBridge.onRouteChanged()
                }
            }

            override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
                if (removedDevices.isNullOrEmpty()) return
                Log.i(AudioSessionBridge.TAG,
                    "AudioDeviceCallback: ${removedDevices.size} device(s) removed")

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    AudioRouteSetter.routeToBestDeviceNow()
                } else {
                    AudioSessionBridge.onRouteChanged()
                }
            }
        }

        am.registerAudioDeviceCallback(callback, AudioSessionBridge.mainHandler)
        AudioSessionBridge.audioDeviceCallback = callback
        Log.d(AudioSessionBridge.TAG, "AudioDeviceCallback registered")
    }

    fun unregisterAudioDeviceCallback() {
        val am = AudioSessionBridge.audioManager ?: return
        AudioSessionBridge.audioDeviceCallback?.let {
            am.unregisterAudioDeviceCallback(it)
        }
        AudioSessionBridge.audioDeviceCallback = null
    }

    fun registerRouteReceiver() {
        val ctx = AudioSessionBridge.context ?: return

        val rcv = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                when (intent?.action) {
                    AudioManager.ACTION_HEADSET_PLUG -> {
                        val state = intent.getIntExtra("state", -1)
                        Log.i(AudioSessionBridge.TAG, "Headset plug: state=$state")
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            AudioRouteSetter.routeToBestDeviceNow()
                        } else {
                            AudioSessionBridge.onRouteChanged()
                        }
                    }
                    AudioManager.ACTION_AUDIO_BECOMING_NOISY -> {
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                            AudioSessionBridge.onRouteChanged()
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
            ctx.registerReceiver(rcv, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            ctx.registerReceiver(rcv, filter)
        }
        AudioSessionBridge.routeReceiver = rcv
        Log.d(AudioSessionBridge.TAG, "Route receiver registered")
    }

    fun unregisterRouteReceiver() {
        val ctx = AudioSessionBridge.context ?: return
        AudioSessionBridge.routeReceiver?.let {
            try {
                ctx.unregisterReceiver(it)
            } catch (e: Exception) {
                Log.w(AudioSessionBridge.TAG, "Error unregistering receiver: ${e.message}")
            }
        }
        AudioSessionBridge.routeReceiver = null
    }

    @Suppress("MissingPermission")
    private fun cacheBtDeviceName(intent: Intent, connected: Boolean) {
        if (!connected) {
            val btDevice = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE,
                    android.bluetooth.BluetoothDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE)
            }
            val disconnectingName = btDevice?.let { AudioDeviceQuery.btDeviceName(it) }
            if (disconnectingName == null || disconnectingName == AudioSessionBridge.connectedBtDeviceName) {
                AudioSessionBridge.connectedBtDeviceName = null
            }
            return
        }
        val btDevice = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE,
                android.bluetooth.BluetoothDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(android.bluetooth.BluetoothDevice.EXTRA_DEVICE)
        }
        if (btDevice != null) {
            AudioSessionBridge.connectedBtDeviceName = AudioDeviceQuery.btDeviceName(btDevice)
        }
    }
}

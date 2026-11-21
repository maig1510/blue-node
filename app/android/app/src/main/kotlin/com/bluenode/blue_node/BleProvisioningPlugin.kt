package com.bluenode.blue_node

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.espressif.provisioning.DeviceConnectionEvent
import com.espressif.provisioning.ESPConstants
import com.espressif.provisioning.ESPDevice
import com.espressif.provisioning.ESPProvisionManager
import com.espressif.provisioning.WiFiAccessPoint
import com.espressif.provisioning.listeners.ResponseListener
import com.espressif.provisioning.listeners.WiFiScanListener
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import org.greenrobot.eventbus.EventBus
import org.greenrobot.eventbus.Subscribe
import org.greenrobot.eventbus.ThreadMode

/**
 * Bridge between Dart (lib/provisioning/esp_ble_transport.dart) and Espressif's
 * esp-idf-provisioning-android library, which implements the BLE transport and
 * protocomm security 1 the node's provisioning component speaks.
 *
 * Deliberately thin: it moves bytes and reports connection state. The pairing
 * flow, the bn-* JSON protocol and the flow's own timeouts live in Dart, where
 * they are unit tested against a fake transport.
 *
 * Channels:
 *  - method `blue_node/provisioning`:
 *      ensureReady() -> "ok" | "denied" | "bluetooth_off" | "unsupported"
 *      connect({address}) -> null, throws on failure
 *      sendCustom({endpoint, data}) -> String (the response, UTF-8)
 *      scanWifi() -> List<Map{ssid, rssi, secured}>
 *      disconnect() -> null
 *    and towards Dart: onDisconnected() when the link drops.
 *    Error codes: busy, not_connected, timeout, disconnected, request_failed,
 *    wifi_scan_failed, connection_failed, unknown_device.
 *  - event `blue_node/provisioning/scan`: one Map{address, name, rssi, mfg}
 *    per advertisement while listened to; the argument is the name prefix.
 *
 * What this class adds on top of the library, because the library does not:
 *  - every request completes. The library has no timeout and never fails a
 *    request whose answer is lost with the link, so a pending request is
 *    failed on disconnect and after [REQUEST_TIMEOUT_MS] / [WIFI_SCAN_TIMEOUT_MS];
 *  - every GATT client is released. The library only calls
 *    BluetoothGatt.close() from disconnectDevice(), so every way a session
 *    ends goes through [releaseDevice] - including a disconnect the node
 *    started - or Android runs out of GATT clients after a few pairings;
 *  - scanning uses BluetoothLeScanner directly: one scan, filtered by the
 *    node's service UUID, for as long as Dart listens. The library's own scan
 *    stops after a fixed 6 s, and restarting it that often runs into
 *    Android's limit of five scan starts per 30 s, after which scans silently
 *    return nothing.
 *
 * Every MethodChannel.Result is completed on the main thread, exactly once,
 * which Flutter requires; the library calls back on its BLE threads, hence
 * `main.post`.
 */
class BleProvisioningPlugin(
    private val activity: Activity,
    messenger: BinaryMessenger,
) : MethodChannel.MethodCallHandler, EventChannel.StreamHandler {

    private val methods = MethodChannel(messenger, METHOD_CHANNEL)
    private val scanEvents = EventChannel(messenger, SCAN_CHANNEL)
    private val main = Handler(Looper.getMainLooper())
    private val manager = ESPProvisionManager.getInstance(activity.applicationContext)

    /** Devices seen by the current scan, by address: connect() needs the object. */
    private val found = mutableMapOf<String, BluetoothDevice>()

    private var scanSink: EventChannel.EventSink? = null
    private var scanPrefix = DEFAULT_PREFIX
    private var scanner: BluetoothLeScanner? = null

    private var device: ESPDevice? = null
    private var pendingConnect: MethodChannel.Result? = null
    private var pendingPermissions: MethodChannel.Result? = null

    /**
     * The one request in flight. The library keeps a single response listener
     * per device, so a second request would steal the first one's answer; the
     * Dart side queues its calls (SerialTransport) and this guards the
     * contract. The id tells a late answer from the library apart from a newer
     * request, once its own request was already failed by a timeout or a
     * disconnect.
     */
    private var pendingRequest: PendingRequest? = null
    private var requestSeq = 0

    private class PendingRequest(val id: Int, val result: MethodChannel.Result)

    private val connectTimeout = Runnable {
        failConnect("timeout", "The node did not answer in time")
        releaseDevice()
    }

    /**
     * The answer is lost: the transport is unusable now (the library still
     * holds its internal lock for it), so the link goes too.
     */
    private val requestTimeout = Runnable {
        Log.w(TAG, "The node did not answer a request, dropping the link")
        failRequest("timeout", "The node did not answer in time")
        if (device != null) {
            releaseDevice()
            methods.invokeMethod("onDisconnected", null)
        }
    }

    // BluetoothLeScanner calls back on the main thread already; the post keeps
    // that an explicit guarantee rather than an assumption.
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            main.post { report(result) }
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            main.post { results.forEach(::report) }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.w(TAG, "BLE scan failed: $errorCode")
            main.post {
                scanner = null
                scanSink?.error("scan_failed", "BLE scan failed (error $errorCode)", null)
            }
        }
    }

    init {
        methods.setMethodCallHandler(this)
        scanEvents.setStreamHandler(this)
        EventBus.getDefault().register(this)
    }

    fun dispose() {
        methods.setMethodCallHandler(null)
        scanEvents.setStreamHandler(null)
        if (EventBus.getDefault().isRegistered(this)) {
            EventBus.getDefault().unregister(this)
        }
        stopScan()
        main.removeCallbacksAndMessages(null)
        // The engine is going away: nobody is left to answer.
        pendingConnect = null
        pendingRequest = null
        device?.disconnectDevice()
        device = null
    }

    // --- MethodChannel ----------------------------------------------------

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "ensureReady" -> ensureReady(result)
            "connect" -> connect(call.argument<String>("address"), result)
            "sendCustom" -> sendCustom(call.argument("endpoint"), call.argument("data"), result)
            "scanWifi" -> scanWifi(result)
            "disconnect" -> {
                disconnect()
                result.success(null)
            }
            else -> result.notImplemented()
        }
    }

    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            // Before Android 12 a BLE scan needs location; the manifest caps it
            // at API 30 so newer phones never ask for it.
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private fun missingPermissions(): List<String> = requiredPermissions().filter {
        ContextCompat.checkSelfPermission(activity, it) != PackageManager.PERMISSION_GRANTED
    }

    private fun ensureReady(result: MethodChannel.Result) {
        val missing = missingPermissions()
        if (missing.isEmpty()) {
            result.success(adapterState())
            return
        }
        if (pendingPermissions != null) {
            result.error("busy", "A permission request is already showing", null)
            return
        }
        pendingPermissions = result
        ActivityCompat.requestPermissions(activity, missing.toTypedArray(), PERMISSION_REQUEST)
    }

    /** Forwarded from MainActivity. */
    fun onRequestPermissionsResult(requestCode: Int) {
        if (requestCode != PERMISSION_REQUEST) return
        val result = pendingPermissions ?: return
        pendingPermissions = null
        result.success(if (missingPermissions().isEmpty()) adapterState() else "denied")
    }

    private fun bluetoothAdapter(): BluetoothAdapter? =
        (activity.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private fun adapterState(): String {
        val adapter = bluetoothAdapter() ?: return "unsupported"
        return if (adapter.isEnabled) "ok" else "bluetooth_off"
    }

    // Permissions are checked by ensureReady(), which Dart always calls first;
    // lint cannot follow that across the channel.
    @SuppressLint("MissingPermission")
    private fun connect(address: String?, result: MethodChannel.Result) {
        val bluetoothDevice = address?.let { found[it] }
        if (bluetoothDevice == null) {
            result.error("unknown_device", "Device $address was not seen by the current scan", null)
            return
        }
        if (pendingConnect != null) {
            result.error("busy", "Already connecting", null)
            return
        }

        stopScan()
        releaseDevice()   // a previous session's link, if one is left
        val espDevice = manager.createESPDevice(
            ESPConstants.TransportType.TRANSPORT_BLE,
            ESPConstants.SecurityType.SECURITY_1,
        )
        device = espDevice
        pendingConnect = result
        main.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
        espDevice.connectBLEDevice(bluetoothDevice, SERVICE_UUID)
    }

    private fun failConnect(code: String, message: String) {
        main.removeCallbacks(connectTimeout)
        pendingConnect?.error(code, message, null)
        pendingConnect = null
    }

    private fun sendCustom(endpoint: String?, data: String?, result: MethodChannel.Result) {
        if (endpoint == null || data == null) {
            result.error("bad_request", "endpoint and data are required", null)
            return
        }
        val espDevice = device
        if (espDevice == null) {
            result.error("not_connected", "No node connected", null)
            return
        }
        val id = beginRequest(result, REQUEST_TIMEOUT_MS) ?: return
        espDevice.sendDataToCustomEndPoint(endpoint, data.toByteArray(Charsets.UTF_8), object : ResponseListener {
            override fun onSuccess(returnData: ByteArray?) {
                main.post {
                    completeRequest(id) { it.success(returnData?.toString(Charsets.UTF_8) ?: "") }
                }
            }

            override fun onFailure(e: Exception) {
                main.post {
                    completeRequest(id) { it.error("request_failed", e.message ?: e.toString(), null) }
                }
            }
        })
    }

    private fun scanWifi(result: MethodChannel.Result) {
        val espDevice = device
        if (espDevice == null) {
            result.error("not_connected", "No node connected", null)
            return
        }
        val id = beginRequest(result, WIFI_SCAN_TIMEOUT_MS) ?: return
        espDevice.scanNetworks(object : WiFiScanListener {
            override fun onWifiListReceived(wifiList: ArrayList<WiFiAccessPoint>?) {
                val networks = wifiList.orEmpty().map {
                    mapOf(
                        "ssid" to it.wifiName,
                        "rssi" to it.rssi,
                        "secured" to (it.security != ESPConstants.WIFI_OPEN.toInt()),
                    )
                }
                main.post { completeRequest(id) { it.success(networks) } }
            }

            override fun onWiFiScanFailed(e: Exception) {
                main.post {
                    completeRequest(id) { it.error("wifi_scan_failed", e.message ?: e.toString(), null) }
                }
            }
        })
    }

    /** Register [result] as the request in flight; null (and answered) when one already is. */
    private fun beginRequest(result: MethodChannel.Result, timeoutMs: Long): Int? {
        if (pendingRequest != null) {
            result.error("busy", "Another request is in flight", null)
            return null
        }
        val id = ++requestSeq
        pendingRequest = PendingRequest(id, result)
        main.postDelayed(requestTimeout, timeoutMs)
        return id
    }

    /** Complete request [id] - unless a timeout or a disconnect already has. */
    private fun completeRequest(id: Int, complete: (MethodChannel.Result) -> Unit) {
        val pending = pendingRequest
        if (pending == null || pending.id != id) return
        pendingRequest = null
        main.removeCallbacks(requestTimeout)
        complete(pending.result)
    }

    private fun failRequest(code: String, message: String) {
        val pending = pendingRequest ?: return
        pendingRequest = null
        main.removeCallbacks(requestTimeout)
        pending.result.error(code, message, null)
    }

    private fun disconnect() {
        failConnect("disconnected", "Disconnected before the connection was up")
        releaseDevice()
    }

    /**
     * End the session: fail a request still waiting for its answer, close the
     * link and release its GATT client (see the class comment).
     */
    private fun releaseDevice() {
        failRequest("disconnected", "The connection to the node was closed")
        device?.disconnectDevice()
        device = null
    }

    /** Connection state from the library's BLE transport. */
    @Subscribe(threadMode = ThreadMode.MAIN)
    fun onDeviceConnectionEvent(event: DeviceConnectionEvent) {
        when (event.eventType) {
            ESPConstants.EVENT_DEVICE_CONNECTED -> {
                main.removeCallbacks(connectTimeout)
                pendingConnect?.success(null)
                pendingConnect = null
            }
            // The library also reports GATT errors on a live link (status 133)
            // as "connection failed": either way the session is over.
            ESPConstants.EVENT_DEVICE_CONNECTION_FAILED -> onLinkLost("Could not connect to the node")
            ESPConstants.EVENT_DEVICE_DISCONNECTED -> onLinkLost("The node closed the connection")
        }
    }

    private fun onLinkLost(message: String) {
        if (pendingConnect != null) {
            failConnect("connection_failed", message)
            releaseDevice()
        } else if (device != null) {
            releaseDevice()
            methods.invokeMethod("onDisconnected", null)
        }
    }

    // --- Scan EventChannel -------------------------------------------------

    override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
        scanSink = events
        scanPrefix = (arguments as? String) ?: DEFAULT_PREFIX
        found.clear()
        startScan()
    }

    override fun onCancel(arguments: Any?) {
        scanSink = null
        stopScan()
    }

    @SuppressLint("MissingPermission")  // see connect()
    private fun startScan() {
        val sink = scanSink ?: return
        if (scanner != null) return
        val leScanner = bluetoothAdapter()?.bluetoothLeScanner
        if (leScanner == null) {
            sink.error("scan_failed", "Bluetooth is off", null)
            return
        }
        // The service UUID is in the advertisement itself (the name and the
        // manufacturer data follow in the scan response), so the Bluetooth
        // chip can filter on it - which also keeps the scan running with the
        // screen off, where Android stops unfiltered ones.
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(ParcelUuid.fromString(SERVICE_UUID)).build(),
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            leScanner.startScan(filters, settings, scanCallback)
            scanner = leScanner
        } catch (e: IllegalStateException) {
            // Bluetooth was switched off between the check and the call.
            sink.error("scan_failed", e.message ?: "Bluetooth is off", null)
        }
    }

    @SuppressLint("MissingPermission")  // see connect()
    private fun stopScan() {
        val leScanner = scanner ?: return
        scanner = null
        try {
            leScanner.stopScan(scanCallback)
        } catch (e: IllegalStateException) {
            // Bluetooth was switched off meanwhile; the scan is gone with it.
            Log.d(TAG, "stopScan: ${e.message}")
        }
    }

    private fun report(result: ScanResult) {
        val sink = scanSink ?: return
        val record = result.scanRecord
        // Without the name this is the advertisement alone, before its scan
        // response arrived; the next result for the same node carries both.
        val name = record?.deviceName ?: return
        if (!name.startsWith(scanPrefix)) return

        found[result.device.address] = result.device
        sink.success(
            mapOf(
                "address" to result.device.address,
                "name" to name,
                "rssi" to result.rssi,
                // Android strips the 2-byte company id; see mfg_data.dart for the rest.
                "mfg" to record.getManufacturerSpecificData(ESPRESSIF_COMPANY_ID),
            ),
        )
    }

    companion object {
        private const val TAG = "BleProvisioning"
        private const val METHOD_CHANNEL = "blue_node/provisioning"
        private const val SCAN_CHANNEL = "blue_node/provisioning/scan"
        private const val DEFAULT_PREFIX = "BN-"
        private const val PERMISSION_REQUEST = 0xB1E

        /** The node's provisioning service, see firmware components/provisioning. */
        private const val SERVICE_UUID = "b1e0de00-6b1d-4c8a-9f3e-5a7c2d4e8f10"

        /** Bluetooth SIG company id the node puts in front of its manufacturer data. */
        private const val ESPRESSIF_COMPANY_ID = 0x02E5

        private const val CONNECT_TIMEOUT_MS = 20_000L

        /**
         * One bn-* round trip (the first one also sets up the security 1
         * session) takes well under a second; this only catches a lost answer.
         */
        private const val REQUEST_TIMEOUT_MS = 10_000L

        /** The node scans every channel before it answers prov-scan. */
        private const val WIFI_SCAN_TIMEOUT_MS = 20_000L
    }
}

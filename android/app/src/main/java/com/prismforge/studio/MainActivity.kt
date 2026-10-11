package com.prismforge.studio

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Rect
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.util.Size
import android.widget.Toast
import android.view.View
import android.util.Patterns
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.prismforge.studio.databinding.ActivityMainBinding
import com.prismforge.studio.databinding.DialogConnectIpBinding
import com.prismforge.studio.networking.ConnectionManager
import com.prismforge.studio.util.Logger
import com.prismforge.studio.video.Camera
import com.google.android.material.dialog.MaterialAlertDialogBuilder

class MainActivity : AppCompatActivity(), ConnectionManager.ConnectionStateCallback {

    private lateinit var viewBinding: ActivityMainBinding

    private val qrscanner = QRScanner()
    private var connectionManager = ConnectionManager.getInstance(this)
    private var camera: Camera? = null

    private var isConnecting = false

    private val TAG = "PrismForge Studio"
    private val PREFS_NAME = "prismforge_prefs"
    private val KEY_LAST_IP = "last_ip"
    private val KEY_LAST_PORT = "last_port"

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action == "android.hardware.usb.action.USB_STATE") {
                val connected = intent.getBooleanExtra("connected", false)
                if (connected && !isConnecting) {
                    Logger.log("MAIN", "USB connected event detected")
                    viewBinding.connectionStatus.text = "USB detected. Tap USB cable to connect."

                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        StudioAppearance.applyTheme(this)
        super.onCreate(savedInstanceState)
        viewBinding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(viewBinding.root)

        viewBinding.logReportButton.setOnClickListener {
            val intent = Intent(this, LogActivity::class.java)
            startActivity(intent)
        }

        viewBinding.btnAppearance.setOnClickListener { StudioAppearance.showPicker(this) }
        setupBottomButtons()
        enableEdgeToEdge()
        viewBinding.root.applyStudioInsets()
        initialize()
    }

    private fun setupBottomButtons() {
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val last = prefs.getString(KEY_LAST_IP, "").orEmpty()
        viewBinding.btnReconnect.visibility = if (last.isNotBlank()) View.VISIBLE else View.GONE
        viewBinding.btnReconnect.setOnClickListener {
            if (!isConnecting) {
                beginConnection("Reconnecting to your computer…")
                connectionManager.connect(last, prefs.getInt(KEY_LAST_PORT, ConnectionDefaults.CONTROL_PORT))
            }
        }
        // Connect via USB button
        viewBinding.btnConnectUsb.setOnClickListener {
            Toast.makeText(this, "Connecting to PC via USB...", Toast.LENGTH_SHORT).show()
            connectUSB()
        }

        // Connect via Wi-Fi / IP button
        viewBinding.btnConnectWifi.setOnClickListener {
            showManualIpDialog()
        }
    }

    private fun showManualIpDialog() {
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val savedIp = prefs.getString(KEY_LAST_IP, "") ?: ""
        val savedPort = prefs.getInt(KEY_LAST_PORT, ConnectionDefaults.CONTROL_PORT)

        val dialogBinding = DialogConnectIpBinding.inflate(layoutInflater)
        if (savedIp.isNotEmpty()) {
            dialogBinding.editIp.setText(savedIp)
        }
        dialogBinding.editPort.setText(savedPort.toString())

        qrscanner.stop()
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Connect to your computer")
            .setView(dialogBinding.root)
            .setPositiveButton("Connect", null)
            .setNegativeButton("Cancel", null)
            .create()
        dialog.setOnDismissListener { if (!isConnecting) qrscanner.start() }
        dialog.setOnShowListener {
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val ip = dialogBinding.editIp.text.toString().trim()
                val port = dialogBinding.editPort.text.toString().toIntOrNull()
                dialogBinding.layoutIp.error = if (Patterns.IP_ADDRESS.matcher(ip).matches()) null else "Enter a valid IP address"
                dialogBinding.layoutPort.error = if (port != null && port in 1..65535) null else "Enter a port from 1 to 65535"
                if (dialogBinding.layoutIp.error == null && dialogBinding.layoutPort.error == null) {
                    prefs.edit().putString(KEY_LAST_IP, ip).putInt(KEY_LAST_PORT, port!!).apply()
                    beginConnection("Connecting to your computer…")
                    connectionManager.connect(ip, port)
                    dialog.dismiss()
                }
            }
        }
        dialog.show()
    }

    private fun beginConnection(message: String) {
        isConnecting = true
        qrscanner.stop()
        viewBinding.connectionStatus.text = message
        viewBinding.btnConnectUsb.isEnabled = false
        viewBinding.btnConnectWifi.isEnabled = false
        viewBinding.btnReconnect.isEnabled = false
        viewBinding.btnAppearance.isEnabled = false
        viewBinding.logReportButton.isEnabled = false
    }

    private fun ready(message: String) {
        isConnecting = false
        viewBinding.connectionStatus.text = message
        viewBinding.btnConnectUsb.isEnabled = true
        viewBinding.btnConnectWifi.isEnabled = true
        viewBinding.btnReconnect.isEnabled = true
        viewBinding.btnAppearance.isEnabled = true
        viewBinding.logReportButton.isEnabled = true
    }

    override fun onResume() {
        super.onResume()
        connectionManager = ConnectionManager.getInstance(this)
        if (!isConnecting) ready("Scan the code in PrismForge Studio on your computer.")
        setupBottomButtons()

        try {
            val filter = IntentFilter("android.hardware.usb.action.USB_STATE")
            registerReceiver(usbReceiver, filter)
        } catch (e: Exception) {
            Logger.log("MAIN", "Could not register USB receiver: ${e.message}")
        }

        if (camera != null && !isConnecting) {
            camera!!.start(Size(1280, 720), CameraSelector.DEFAULT_BACK_CAMERA)
            connectWIFI()
        }
    }

    override fun onPause() {
        super.onPause()
        qrscanner.stop()
        try {
            unregisterReceiver(usbReceiver)
        } catch (_: Exception) {}
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 1000) {
            if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                initialize()
            } else {
                MaterialAlertDialogBuilder(this)
                    .setTitle("Camera Permission Required")
                    .setMessage("Please enable camera permission in settings and restart the app.")
                    .setPositiveButton("Go to settings") { _, _ ->
                        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
                            data = Uri.fromParts("package", packageName, null)
                        }
                        startActivity(intent)
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
        }
    }

    override fun onConnectionSuccessful(connectionMode: ConnectionManager.Mode) {
        isConnecting = false
        qrscanner.stop()
        Logger.log("MAIN", "Connection successful $connectionMode")

        runOnUiThread {
            val session = connectionManager.session
            if (session == null) {
                ready("Connection closed. Tap Connect USB or reconnect over Wi-Fi.")
                return@runOnUiThread
            }
            camera?.stop()
            val intent = Intent(this, StreamActivity::class.java)
                .putExtra(StreamActivity.SESSION_ATTEMPT, session.attempt)
            startActivity(intent)
        }
    }

    override fun onConnectionFailed(connectionMode: ConnectionManager.Mode) {
        runOnUiThread {
            ready(if (connectionMode == ConnectionManager.Mode.USB) "USB unavailable. Check USB debugging or scan a QR code." else "Could not connect. Check the address and local network.")
            if (connectionMode == ConnectionManager.Mode.USB) {
                Logger.log("MAIN", "USB connection attempt failed")
                Toast.makeText(this, "USB not connected. Check cable or tap 'Connect USB' to retry.", Toast.LENGTH_LONG).show()
                // Keep QR scanner running for Wi-Fi fallback
                connectWIFI()
            } else {
                Logger.log("MAIN", "Wi-Fi connection failed")
                Toast.makeText(this, "Wi-Fi connection failed. Check IP & port, or tap 'Enter IP'.", Toast.LENGTH_LONG).show()
                connectWIFI()
            }
        }
    }

    private fun checkPermissions(): Boolean {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.CAMERA), 1000)
            return false
        }
        return true
    }

    private fun initialize() {
        if (checkPermissions()) {
            camera = Camera(
                viewBinding.viewFinder.surfaceProvider,
                ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888,
                ::processImage,
                this,
                this
            )
            camera!!.start(Size(1280, 720), CameraSelector.DEFAULT_BACK_CAMERA)

            // Start QR scanning for Wi-Fi by default
            connectWIFI()


        }
    }

    private fun processImage(imageProxy: ImageProxy) {
        qrscanner.launchScanTask(imageProxy, Rect()) { result ->
            runOnUiThread {
                if (result != null && !isConnecting) {
                    qrscanner.stop()
                    MaterialAlertDialogBuilder(this)
                        .setTitle("Connect via Wi-Fi")
                        .setMessage("Connect to ${result.address}:${result.port}?")
                        .setPositiveButton("Connect") { _, _ ->
                            val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                            prefs.edit()
                                .putString(KEY_LAST_IP, result.address)
                                .putInt(KEY_LAST_PORT, result.port)
                                .apply()

                            beginConnection("Connecting to your computer…")
                            connectionManager.connect(result.address, result.port)
                        }
                        .setNegativeButton("Cancel") { _, _ -> qrscanner.start() }
                        .show()
                }
            }
        }
    }

    private fun hasUsbConnection(): Boolean {
        return try {
            val intent = applicationContext.registerReceiver(
                null,
                IntentFilter("android.hardware.usb.action.USB_STATE")
            )
            intent?.getBooleanExtra("connected", false) == true
        } catch (_: Exception) {
            false
        }
    }

    private fun hasWifiConnection(): Boolean {
        val connectivityManager = applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = connectivityManager.activeNetwork ?: return false
        val capabilities = connectivityManager.getNetworkCapabilities(network) ?: return false
        return capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
    }

    private fun connectUSB() {
        if (isConnecting) return
        beginConnection("Connecting over USB…")
        connectionManager.connect(ConnectionDefaults.CONTROL_PORT)
    }

    private fun connectWIFI() {
        qrscanner.start()
    }
}

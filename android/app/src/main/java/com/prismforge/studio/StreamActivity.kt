package com.prismforge.studio

import android.animation.ObjectAnimator
import android.animation.ValueAnimator
import android.os.Bundle
import android.content.pm.ActivityInfo
import androidx.activity.enableEdgeToEdge
import androidx.core.content.ContextCompat
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.button.MaterialButton
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.prismforge.studio.databinding.ActivityStreamBinding
import com.prismforge.studio.networking.ConnectionManager
import com.prismforge.studio.networking.ConnectionSession
import com.prismforge.studio.networking.SessionHandoff
import com.prismforge.studio.networking.PacketType
import com.prismforge.studio.rtsp.StreamOptions
import com.prismforge.studio.rtsp.Streamer
import com.prismforge.studio.util.Logger

class StreamActivity : AppCompatActivity(), SurfaceHolder.Callback, ConnectionManager.ConnectionStateCallback {

    companion object { const val SESSION_ATTEMPT = "prismforge.connection.attempt" }

    private val TAG = "PrismForge Studio"

    private lateinit var viewBinding: ActivityStreamBinding

    private val connectionManager = ConnectionManager.getInstance()
    private var connectionSession: ConnectionSession? = null
    private lateinit var streamer: Streamer

    private var isTorchOn = false
    private var isDimmed = false
    private var pulseAnimator: ObjectAnimator? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        StudioAppearance.applyTheme(this)
        super.onCreate(savedInstanceState)
        val session = SessionHandoff.accept(intent.getLongExtra(SESSION_ATTEMPT, -1), connectionManager.session)
        if (session == null) {
            Logger.log("STREAM", "Connection closed or replaced before streaming screen opened")
            finish()
            return
        }
        connectionSession = session
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        viewBinding = ActivityStreamBinding.inflate(layoutInflater)
        setContentView(viewBinding.root)


        enableEdgeToEdge()
        viewBinding.streamChrome.applyStudioInsets()
        streamer = Streamer(StreamOptions(), this, viewBinding.surfaceView, session)
        streamer.onStateChanged = { state ->
            if (!isFinishing && !isDestroyed) {
                viewBinding.liveStatusText.text = state
                viewBinding.resolutionText.text = streamer.captureDescription
                viewBinding.liveDot.imageTintList = android.content.res.ColorStateList.valueOf(StudioAppearance.color(this,
                    if (state == "Streaming") R.color.studio_green else R.color.studio_muted))
            }
        }
        viewBinding.connectionTypeText.text = when (connectionManager.currentMode) {
            ConnectionManager.Mode.USB -> "USB · local connection"
            ConnectionManager.Mode.WIFI -> "Wi-Fi · local connection"
            null -> "Connection unavailable"
        }
        setupControls()
        val attached = connectionManager.attachSession(session.attempt, this) { buffer, bytes ->
            // Own the packet and marshal camera/UI actions to the main thread.
            val packet = buffer.copyOf(bytes)
            runOnUiThread { if (!isFinishing && !isDestroyed && connectionManager.isCurrentSession(session.attempt) && packet.isNotEmpty()) {
                runCatching { onBytesReceived(packet, packet.size) }.onFailure {
                    Logger.log("CONTROL", "Camera command failed: ${it.message}")
                    viewBinding.liveStatusText.text = "Camera setting unavailable"
                }
            } }
        }
        if (!attached) { finish(); return }
        // No surface callback can reach an uninitialized Streamer.
        viewBinding.surfaceView.holder.addCallback(this)
    }

    private fun setupControls() {
        viewBinding.btnMore.setOnClickListener { showControls() }
        // Switch Camera Button
        viewBinding.btnSwitchCamera.setOnClickListener {
            streamer.switchCamera()
            viewBinding.btnSwitchCamera.contentDescription = streamer.cameraDescription
        }

        // Torch Toggle Button
        viewBinding.btnTorch.setOnClickListener {
            isTorchOn = !isTorchOn
            runCatching { streamer.setFlash(isTorchOn) }.onFailure {
                isTorchOn = false
                Toast.makeText(this, "Torch is unavailable on this camera", Toast.LENGTH_SHORT).show()
            }
            viewBinding.btnTorch.text = if (isTorchOn) "Torch on" else "Torch"
        }

        // Eco Screen Dim Button (OLED battery saver & thermal protection)
        viewBinding.btnDimScreen.setOnClickListener {
            setDimMode(true)
        }

        // Tap overlay anywhere to wake up
        viewBinding.dimOverlay.setOnClickListener {
            setDimMode(false)
        }

        // Stop Stream Button
        viewBinding.btnStopStream.setOnClickListener {
            finish()
        }
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        refreshAppearance()
    }

    fun refreshAppearance() {
        // Keep the camera and GL surface alive while recoloring only the controls.
        val surface = StudioAppearance.color(this, R.color.studio_surface)
        val text = StudioAppearance.color(this, R.color.studio_text)
        val muted = StudioAppearance.color(this, R.color.studio_muted)
        viewBinding.topStatusHud.backgroundTintList = android.content.res.ColorStateList.valueOf(surface)
        viewBinding.bottomActionHud.backgroundTintList = android.content.res.ColorStateList.valueOf(surface)
        viewBinding.liveStatusText.setTextColor(text)
        viewBinding.resolutionText.setTextColor(muted)
        viewBinding.connectionTypeText.setTextColor(StudioAppearance.color(this, R.color.studio_accent))
        listOf(viewBinding.btnMore, viewBinding.btnSwitchCamera, viewBinding.btnTorch, viewBinding.btnDimScreen, viewBinding.btnStopStream).forEach {
            it.setTextColor(text)
            it.iconTint = android.content.res.ColorStateList.valueOf(text)
            it.backgroundTintList = android.content.res.ColorStateList.valueOf(StudioAppearance.color(this, R.color.studio_raised))
        }
        androidx.core.view.ViewCompat.requestApplyInsets(viewBinding.streamChrome)
    }

    private fun showControls() {
        val sheet = BottomSheetDialog(this)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val p = (24 * resources.displayMetrics.density).toInt()
            setPadding(p, p, p, p)
        }
        layout.addView(TextView(this).apply { text = "Camera controls"; textSize = 24f })
        layout.addView(TextView(this).apply {
            text = "${streamer.cameraDescription}\n${streamer.captureDescription}\nChange quality, framing and effects from your computer."
            textSize = 14f
            setPadding(0, 16, 0, 24)
        })
        fun action(title: String, run: () -> Unit) {
            layout.addView(MaterialButton(this).apply {
                text = title
                minHeight = (52 * resources.displayMetrics.density).toInt()
                setOnClickListener { runCatching { run() }.onFailure { Toast.makeText(context, "Control unavailable", Toast.LENGTH_SHORT).show() }; sheet.dismiss() }
            })
        }
        action("Switch front / rear camera") { streamer.switchCamera() }
        action("Rotate video 90°") { streamer.rotate(90) }
        action("Dim screen while streaming") { setDimMode(true) }
        action("Appearance: system, dark, light or glass") { StudioAppearance.showPicker(this) }
        action("Stop streaming") { finish() }
        sheet.setContentView(layout)
        sheet.show()
    }

    private fun setDimMode(enable: Boolean) {
        isDimmed = enable
        val lp = window.attributes
        if (enable) {
            viewBinding.dimOverlay.visibility = View.VISIBLE
            lp.screenBrightness = 0.01f
        } else {
            viewBinding.dimOverlay.visibility = View.GONE
            lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
        window.attributes = lp
    }

    private fun startLivePulse() {
        pulseAnimator = ObjectAnimator.ofFloat(viewBinding.liveDot, "alpha", 1.0f, 0.2f).apply {
            duration = 900
            repeatCount = ValueAnimator.INFINITE
            repeatMode = ValueAnimator.REVERSE
            start()
        }
    }

    override fun onDestroy() {
        if (::streamer.isInitialized) {
            streamer.onStateChanged = null
            streamer.stop()
        }
        connectionSession?.let { connectionManager.disconnect(it.attempt) }
        super.onDestroy()
        pulseAnimator?.cancel()
        if (::viewBinding.isInitialized) setDimMode(false)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (::streamer.isInitialized && holder.surface != null && holder.surface.isValid) {
            runCatching { streamer.resumeSurface() }.onFailure { viewBinding.liveStatusText.text = "Camera unavailable" }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) { }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (::streamer.isInitialized) streamer.suspendSurface()
    }

    override fun onDisconnected() {
        runOnUiThread {
            Toast.makeText(this, "Connection closed by server", Toast.LENGTH_LONG).show()
        }
        Logger.log("STREAM", "TCP server disconnected")
        if (::streamer.isInitialized) streamer.stop()
        finish()
    }

    private fun onBytesReceived(buffer: ByteArray, bytes: Int) {
        val type = buffer[0]

        when (type) {
            PacketType.ACTIVATION -> {
                val options = StreamOptions.deserialize(buffer, bytes)
                streamer.startStream(options)
                runOnUiThread {
                    viewBinding.resolutionText.text = "${options.width}x${options.height} • ${options.fps}fps"
                    viewBinding.liveStatusText.text = "Starting video"
                }
            }
            PacketType.CAMERA -> {
                streamer.switchCamera()
                runOnUiThread {
                    viewBinding.btnSwitchCamera.contentDescription = streamer.cameraDescription
                }
            }
            PacketType.RESOLUTION -> {
                val width = (buffer[1].toInt() and 0xFF) or (buffer[2].toInt() shl 8)
                val height = (buffer[3].toInt() and 0xFF) or (buffer[4].toInt() shl 8)
                streamer.setResolution(width, height)
                runOnUiThread {
                    viewBinding.resolutionText.text = "${width}x${height}"
                }
            }
            PacketType.ROTATION -> {
                val degrees = buffer[1].toInt()
                streamer.rotate(degrees)
            }
            PacketType.EFFECT_FILTER -> {
                val filterName = String(buffer, 2, (buffer[1].toInt() and 255), Charsets.UTF_8)
                streamer.applyEffectFilter(filterName)
            }
            PacketType.CORRECTION_FILTER -> {
                val filterName = String(buffer, 2, (buffer[1].toInt() and 255), Charsets.UTF_8)
                val value = (buffer[2 + (buffer[1].toInt() and 255)].toInt() and 255)
                streamer.applyCorrectionFilter(filterName, value)
            }
            PacketType.BITRATE -> {
                val bitrate = (buffer[1].toInt() and 0xFF) or (buffer[2].toInt() shl 8)
                streamer.setBitrate(bitrate)
            }
            PacketType.ADAPTIVE_BITRATE -> {
                val min = (buffer[1].toInt() and 0xFF) or (buffer[2].toInt() shl 8)
                val max = (buffer[3].toInt() and 0xFF) or (buffer[4].toInt() shl 8)
                streamer.setAdaptiveBitrate(min, max)
            }
            PacketType.STABILIZATION -> {
                streamer.setStabilization(buffer[1].toInt() == 1)
            }
            PacketType.FLASH -> {
                isTorchOn = buffer[1].toInt() == 1
                streamer.setFlash(isTorchOn)
                runOnUiThread {
                    viewBinding.btnTorch.text = if (isTorchOn) "Torch on" else "Torch"
                }
            }
            PacketType.FOCUS -> {
                streamer.setFocus(buffer[1].toInt())
            }
            PacketType.CODEC -> {
                streamer.setH265Codec(buffer[1].toInt() == 1)
            }
            PacketType.FPS -> {
                val fps = buffer[1].toInt()
                streamer.setFps(fps)
                runOnUiThread {
                    val currentText = viewBinding.resolutionText.text.toString()
                    val parts = currentText.split(" • ")
                    if (parts.isNotEmpty()) {
                        viewBinding.resolutionText.text = "${parts[0]} • ${fps}fps"
                    }
                }
            }
            PacketType.ZOOM -> {
                val factor = (buffer[1].toInt() and 0xFF) or
                        ((buffer[2].toInt() and 0xFF) shl 8) or
                        ((buffer[3].toInt() and 0xFF) shl 16) or
                        ((buffer[4].toInt() and 0xFF) shl 24)
                streamer.setZoom(Float.fromBits(factor))
            }
            PacketType.FLIP -> {
                val axis = buffer[1].toInt()
                streamer.flip(if (axis == 0) StreamOptions.FlipAxis.VERTICAL else StreamOptions.FlipAxis.HORIZONTAL)
            }
        }
    }
}

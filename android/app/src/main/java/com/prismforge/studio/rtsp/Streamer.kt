package com.prismforge.studio.rtsp

import android.content.Context
import android.app.Activity
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Display
import android.view.ViewOverlay
import com.prismforge.studio.networking.ConnectionManager
import com.prismforge.studio.networking.ConnectionSession
import com.prismforge.studio.networking.DeviceDescriptor
import com.prismforge.studio.networking.ErrorReport
import com.prismforge.studio.util.Logger
import com.prismforge.studio.video.filters.FilterAdjuster
import com.prismforge.studio.video.filters.FilterRepository
import com.prismforge.studio.video.filters.custom.HorizontalFlipFilterRender
import com.prismforge.studio.video.filters.custom.VerticalFlipFilterRender
import com.prismforge.studio.video.getSupportedResolutions
import com.prismforge.studio.video.RotationMath
import com.prismforge.studio.video.VideoPreparation
import com.pedro.common.ConnectChecker
import com.pedro.common.VideoCodec
import com.pedro.encoder.input.gl.render.filters.BaseFilterRender
import com.pedro.encoder.input.video.CameraHelper
import com.pedro.encoder.input.video.CameraOpenException
import com.pedro.library.util.BitrateAdapter
import com.pedro.library.view.OpenGlView
import com.pedro.rtspserver.RtspServerCamera2
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.util.logging.Filter

class Streamer(
    private var options: StreamOptions,
    private val context: Context,
    openGlView: OpenGlView,
    private val session: ConnectionSession,
) : ConnectChecker {

    companion object {
        const val PORT = com.prismforge.studio.ConnectionDefaults.RTSP_PORT
        const val URL = "rtsp://localhost:$PORT/live"
    }

    var onStateChanged: ((String) -> Unit)? = null
    val captureDescription: String get() = "${options.width} × ${options.height} · ${options.fps} fps requested"
    val cameraDescription: String get() = if (options.camera == CameraHelper.Facing.BACK) "Rear camera" else "Front camera"
    private fun publishState(state: String) { Handler(Looper.getMainLooper()).post { onStateChanged?.invoke(state) } }
    private val TAG = "PrismForge Studio"

    private val rtspServerCamera2 = RtspServerCamera2(openGlView, this, PORT)
    private val connectionManager = ConnectionManager.getInstance()

    private var bitrateAdapter = BitrateAdapter {
        if (options.adaptiveBitrateEnabled) {
            rtspServerCamera2.setVideoBitrateOnFly(it)
        }
    }

    init {
        CameraDiagnostics.attach(context, rtspServerCamera2)
        val connectionManager = ConnectionManager.getInstance()
        val localIpAddress = session.localIpAddress

        getSupportedResolutions(context) { back, front ->
            val descriptor = DeviceDescriptor(
                android.os.Build.MODEL,
                URL.replace("localhost", localIpAddress),
                back,
                front,
                FilterRepository.filters
            )

            connectionManager.sendDescriptor(descriptor, session.attempt)
        }
    }

    private fun sendErrorReport(report: ErrorReport) {
        connectionManager.sendErrorReport(report, session.attempt)
    }

    fun stop() {
        publishState("Stopped")
        rtspServerCamera2.stopPreview()
        if (rtspServerCamera2.isStreaming) {
            rtspServerCamera2.stopStream()
        }
    }

    /**
     * Switch camera front <-> back
     */
    fun switchCamera() {
        try {
            rtspServerCamera2.switchCamera()
            // Make sure local state is synced correctly by manually updating it
            if (options.camera == CameraHelper.Facing.BACK) {
                options.camera = CameraHelper.Facing.FRONT
            } else {
                options.camera = CameraHelper.Facing.BACK
            }
        } catch (e: CameraOpenException) {
            Logger.log("STREAMER", "Can't switch camera. Current resolution ({${options.width}, ${options.height}) not supported")
            sendErrorReport(ErrorReport(
                ErrorReport.Severity.ERROR,
                "Can't switch camera",
                "Resolution ({${options.width}, ${options.height}) not supported"
            ))
        }
    }

    /**
     * Set a new resolution. Requires a stream restart
     */
    fun setResolution(width: Int, height: Int) {
        if (options.width == width && options.height == height)
            return

        Logger.log("STREAMER", "Changing resolution to ${width}x${height}...")
        options.width = width
        options.height = height
        options.bitrate = calculateOptimalBitrate(width, height, options.fps)
        restartStream()
    }

    fun rotate(degrees: Int) {
        if (degrees % 90 != 0) {
            Logger.log("STREAMER", "WARNING: Invalid rotation $degrees; clamped to 0")
            return
        }
        options.rotation = RotationMath.resolve(options.rotation + degrees)
        rtspServerCamera2.glInterface.setStreamRotation(options.rotation)
    }

    /**
     * Apply an effect filter. Only one active at a time
     * @param filterName Filter's name
     */
    fun applyEffectFilter(filterName: String) {
        val category = FilterRepository.getCategory(filterName)
        if (category == FilterRepository.Category.CORRECTION) {
            return
        }

        CoroutineScope(Dispatchers.Main).launch {
            // Remove the current filter if there is one
            options.activeEffectFilter?.let { rtspServerCamera2.glInterface.removeFilter(it) }
            // Only apply the new requested effect filter if it is different than the NONE filter
            // Applying the NONE filter would completely remove all filters, including the correction filters
            // This behaviour results in deleting the effect filter but keeping the corrections
            if (category != FilterRepository.Category.NONE) {
                val filter = FilterRepository.create(filterName)
                if (filter != null) {
                    rtspServerCamera2.glInterface.addFilter(filter)
                    options.activeEffectFilter = filter
                }
            } else {
                options.activeEffectFilter = null
            }
        }
    }

    /**
     * Apply a correction filter. Multiple filters can be active at once.
     * If the filter was already applied it is adjusted with new value
     */
    fun applyCorrectionFilter(filterName: String, value: Int) {
        val category = FilterRepository.getCategory(filterName)
        val fclass = FilterRepository.getClass(filterName)

        if (category != FilterRepository.Category.CORRECTION || fclass == null) {
            return
        }

        val existingFilter = options.activeCorrectionFilters.find { it::class.java == fclass }
        if (existingFilter != null) {
            if (value == 50) {
                options.activeCorrectionFilters.remove(existingFilter)
                rtspServerCamera2.glInterface.removeFilter(existingFilter)
            } else {
                FilterAdjuster.adjust(existingFilter, value)
            }
        } else if (value != 50) {
            val filter = FilterRepository.create(filterName)
            if (filter != null) {
                CoroutineScope(Dispatchers.Main).launch {
                    FilterAdjuster.adjust(filter, value)
                    options.activeCorrectionFilters.add(filter)
                    rtspServerCamera2.glInterface.addFilter(filter)
                }
            }
        }
    }

    /**
     * Set a new target fps. Requires a stream restart
     */
    fun setFps(fps: Int) {
        if (options.fps == fps)
            return

        Logger.log("STREAMER", "Changing fps to ${fps}...")
        options.fps = fps
        restartStream()
    }

    fun setZoom(zoom: Float) {
        rtspServerCamera2.zoom = zoom
    }

    fun flip(axis: StreamOptions.FlipAxis) {
        CoroutineScope(Dispatchers.Main).launch {
            when (axis) {
                StreamOptions.FlipAxis.HORIZONTAL -> options.hFlipFilter = toggleFilter(options.hFlipFilter) { HorizontalFlipFilterRender() } as HorizontalFlipFilterRender?
                StreamOptions.FlipAxis.VERTICAL -> options.vFlipFilter = toggleFilter(options.vFlipFilter) { VerticalFlipFilterRender() } as VerticalFlipFilterRender?
            }
        }
    }

    private fun toggleFilter(
        currentFilter: BaseFilterRender?,
        createFilter: () -> BaseFilterRender
    ): BaseFilterRender? {
        return if (currentFilter != null) {
            // If it exists, remove it and return null (to clear the variable)
            rtspServerCamera2.glInterface.removeFilter(currentFilter)
            null
        } else {
            // If it's null, create new one, add it, and return it
            val newFilter = createFilter()
            rtspServerCamera2.glInterface.addFilter(newFilter)
            newFilter
        }
    }

    /**
     * Set a new bitrate
     */
    fun setBitrate(bitrate: Int) {
        options.adaptiveBitrateEnabled = false
        options.bitrate = bitrate * 1024
        rtspServerCamera2.setVideoBitrateOnFly(options.bitrate)
    }

    fun setAdaptiveBitrate(min: Int, max: Int) {
        options.adaptiveBitrateEnabled = true
        options.adaptiveBitrateMin = min * 1024
        options.adaptiveBitrateMax = max * 1024

        bitrateAdapter.setMaxBitrate(options.adaptiveBitrateMax)
        rtspServerCamera2.setVideoBitrateOnFly(options.adaptiveBitrateMax)
    }

    fun setStabilization(enabled: Boolean) {
        options.stabilization = enabled
        when (enabled) {
            true -> rtspServerCamera2.enableVideoStabilization()
            false -> {
                rtspServerCamera2.disableVideoStabilization()
                rtspServerCamera2.disableOpticalVideoStabilization()
            }
        }
    }

    fun setFlash(enabled: Boolean) {
        when (enabled) {
            true -> rtspServerCamera2.enableLantern()
            false -> rtspServerCamera2.disableLantern()
        }
    }

    fun setFocus(mode: Int) {
        options.focusMode = mode
        when (mode) {
            0 -> rtspServerCamera2.enableAutoFocus()
            1 -> rtspServerCamera2.disableAutoFocus()
        }
    }

    fun setH265Codec(enabled: Boolean) {
        options.h265Enabled = enabled
        val codec = if (enabled) VideoCodec.H265 else VideoCodec.H264
        rtspServerCamera2.setVideoCodec(codec)

        // Only restart if we are actually streaming
        if (rtspServerCamera2.isStreaming) {
            restartStream()
        }
    }

    private var resumeOnSurface = false
    fun suspendSurface() {
        resumeOnSurface = rtspServerCamera2.isStreaming
        stop()
    }
    fun resumeSurface() {
        startPreview()
        if (resumeOnSurface) { resumeOnSurface = false; startStreamInternal() }
    }

    fun startPreview() {
        if (!rtspServerCamera2.isOnPreview) {
            val orientation = cameraOrientation()
            rtspServerCamera2.startPreview(options.camera, options.width, options.height, orientation)
        }
    }

    private fun cameraTask(action: () -> Unit) {
        Handler(Looper.getMainLooper()).post {
            runCatching(action).onFailure {
                publishState("Camera setting unavailable. Try another quality profile.")
                Logger.log("STREAMER", "Camera update failed: ${it.message}")
            }
        }
    }

    fun startStream(newOptions: StreamOptions) {
        val oldOptions = this.options
        newOptions.rotation = oldOptions.rotation
        newOptions.hFlipFilter = oldOptions.hFlipFilter
        newOptions.vFlipFilter = oldOptions.vFlipFilter
        this.options = newOptions

        cameraTask {
            publishState("Applying camera settings")
            if (rtspServerCamera2.isStreaming) rtspServerCamera2.stopStream()
            oldOptions.activeEffectFilter?.let { rtspServerCamera2.glInterface.removeFilter(it) }
            oldOptions.activeCorrectionFilters.forEach { rtspServerCamera2.glInterface.removeFilter(it) }
            val needPreviewRestart = (oldOptions.width != newOptions.width) ||
                    (oldOptions.height != newOptions.height) ||
                    (oldOptions.camera != newOptions.camera)

            if (needPreviewRestart && rtspServerCamera2.isOnPreview) {
                rtspServerCamera2.stopPreview()
            }

            if (!rtspServerCamera2.isOnPreview) {
                startPreview()
            }

            applyOptionsToStream(newOptions)
            startStreamInternal()
        }
    }

    private fun startStreamInternal() {
        if (!rtspServerCamera2.isStreaming) {
            try {
                Logger.log(
                    "STREAMER",
                    "Preparing ${options.width}x${options.height} @ ${options.fps} fps bitrate=${options.bitrate}"
                )
                val orientation = cameraOrientation()
                Logger.log("STREAMER", "Encoder rotation=$orientation; keyframe interval=${VideoPreparation.KEYFRAME_INTERVAL_SECONDS}s")
                val prepared = VideoPreparation.prepare(
                    width = options.width,
                    height = options.height,
                    fps = options.fps,
                    bitrate = options.bitrate,
                    rotationDegrees = orientation,
                    backend = rtspServerCamera2::prepareVideo
                )

                Logger.log("STREAMER", "prepareVideo returned = $prepared")

                if (!prepared) {
                    publishState("Unsupported quality. Choose another profile on your computer.")
                    Logger.log(
                        "STREAMER",
                        "Encoder rejected ${options.width}x${options.height} @ ${options.fps}fps"
                    )
                    return
                }

                publishState("Camera ready")
                rtspServerCamera2.startStream(URL)
                Logger.log("STREAMER", "Stream started")
            } catch (e: Exception) {
                Logger.log("STREAMER", e.stackTraceToString())
                Logger.log("STREAMER", "Error preparing stream")
                publishState("Could not start video. Try a lower quality profile.")
                sendErrorReport(ErrorReport(
                    ErrorReport.Severity.ERROR,
                    "Can't start stream",
                    "({${options.width}, ${options.height}) @${options.fps}fps not supported"
                ))
            }
        }
    }

    /**
     * Apply initial options
     */
    private fun applyOptionsToStream(options: StreamOptions) {
        if (options.adaptiveBitrateEnabled) {
            bitrateAdapter.setMaxBitrate(options.adaptiveBitrateMax)
        } else {
            rtspServerCamera2.setVideoBitrateOnFly(options.bitrate)
        }

        setStabilization(options.stabilization)
        setFocus(options.focusMode)
        rtspServerCamera2.setVideoCodec(if (options.h265Enabled) VideoCodec.H265 else VideoCodec.H264)
        if (options.rotation % 90 != 0) {
            Logger.log("STREAMER", "WARNING: Invalid stored rotation ${options.rotation}; clamped to 0")
            options.rotation = 0
        }
        options.rotation = RotationMath.resolve(options.rotation)
        rtspServerCamera2.glInterface.setStreamRotation(options.rotation)

        options.activeEffectFilter?.let { rtspServerCamera2.glInterface.addFilter(it) }
        options.activeCorrectionFilters.forEach {
            rtspServerCamera2.glInterface.addFilter(it)
        }
    }

    private fun cameraOrientation(): Int {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val front = options.camera == CameraHelper.Facing.FRONT
        val facing = if (front) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
        val currentId = rtspServerCamera2.currentCameraId
        val id = manager.cameraIdList.firstOrNull {
            it == currentId && manager.getCameraCharacteristics(it)[CameraCharacteristics.LENS_FACING] == facing
        } ?: manager.cameraIdList.first {
            manager.getCameraCharacteristics(it)[CameraCharacteristics.LENS_FACING] == facing
        }
        val sensor = requireNotNull(manager.getCameraCharacteristics(id)[CameraCharacteristics.SENSOR_ORIENTATION])
        @Suppress("DEPRECATION")
        val display = (context as Activity).windowManager.defaultDisplay.rotation
        return RotationMath.resolve(sensor, display, front)
    }

    private fun restartStream() {
        cameraTask {
            val wasStreaming = rtspServerCamera2.isStreaming

            rtspServerCamera2.stopStream()
            rtspServerCamera2.stopPreview()

            startPreview()
            if (wasStreaming) {
                startStreamInternal()
            }
        }
    }

    /**
     * Calculate the optimal bitrate for given resolution and fps.
     * Uses 1080p 30fps @ 4000Mbps as a standard reference
     */
    private fun calculateOptimalBitrate(width: Int, height: Int, fps: Int): Int {
        val targetPixelCount = width.toLong() * height.toLong() * fps
        val referencePixelCount = 1920L * 1080L * 30L
        val referenceBitrate = 6000 * 1024L

        val calculatedBitrate =
            (targetPixelCount.toDouble() / referencePixelCount.toDouble()) * referenceBitrate
        val minBitrate = 500 * 1024
        val maxBitrate = 12000 * 1024

        return calculatedBitrate.toInt().coerceIn(minBitrate, maxBitrate)
    }

    override fun onNewBitrate(bitrate: Long) {
        bitrateAdapter.adaptBitrate(bitrate)
    }

    override fun onAuthError() {}

    override fun onAuthSuccess() {}

    override fun onConnectionFailed(reason: String) { publishState("Video interrupted. Reconnect from your computer.") }

    override fun onConnectionStarted(url: String) {}

    override fun onConnectionSuccess() { publishState("Streaming") }

    override fun onDisconnect() { publishState("Waiting for computer") }
}

package com.prismforge.studio.rtsp

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.ImageFormat
import android.media.Image
import android.os.Handler
import android.os.Looper
import com.prismforge.studio.util.Logger
import com.pedro.rtspserver.RtspServerCamera2
import com.pedro.encoder.input.video.Camera2ApiManager
import java.io.File
import java.lang.ref.WeakReference
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** Explicit ADB-only, debug-only captures. Nothing is captured during ordinary app use. */
internal object CameraDiagnostics {
    private var active = WeakReference<RtspServerCamera2>(null)
    private val writer = Executors.newSingleThreadExecutor()
    private val busy = AtomicBoolean(false)
    fun attach(context: Context, camera: RtspServerCamera2) { active = WeakReference(camera) }

    fun capture(context: Context, tag: String, raw: Boolean) {
        val camera = active.get() ?: return
        if (!busy.compareAndSet(false, true)) return
        val directory = File(context.filesDir, "pipeline-captures").apply { mkdirs() }
        fun save(name: String, bitmap: Bitmap) {
            writer.execute {
                try {
                    File(directory, "$tag-$name.png").outputStream().use {
                        bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
                    }
                    Logger.log("CAPTURE", "Saved $tag-$name.png (${bitmap.width}x${bitmap.height})")
                } finally { bitmap.recycle() }
            }
        }
        camera.glInterface.takePhoto { bitmap ->
            save("encoder-input", bitmap)
            if (!raw) busy.set(false)
        }
        if (raw) {
            val first = AtomicBoolean(true)
            // Adds a Camera2 output and therefore reconfigures the capture session. Document this in results.
            camera.addImageListener(1280, 720, ImageFormat.YUV_420_888, 2, true, object : Camera2ApiManager.ImageCallback {
                override fun onImageAvailable(image: Image) {
                if (first.compareAndSet(true, false)) {
                    runCatching {
                        val plane = image.planes[0]
                        val buffer = plane.buffer.duplicate()
                        val pixels = IntArray(image.width * image.height)
                        for (y in 0 until image.height) for (x in 0 until image.width) {
                            val luma = buffer.get(y * plane.rowStride + x * plane.pixelStride).toInt() and 255
                            pixels[y * image.width + x] = Color.rgb(luma, luma, luma)
                        }
                        save("raw-camera-luma", Bitmap.createBitmap(pixels, image.width, image.height, Bitmap.Config.ARGB_8888))
                    }.onFailure { Logger.log("CAPTURE", "Raw capture failed: ${it.message}") }
                    Handler(Looper.getMainLooper()).post {
                        camera.removeImageListener()
                        busy.set(false)
                    }
                }
                }
            })
            Handler(Looper.getMainLooper()).postDelayed({
                if (first.compareAndSet(true, false)) {
                    camera.removeImageListener()
                    busy.set(false)
                    Logger.log("CAPTURE", "Raw capture timed out")
                }
            }, 5000)
        }
    }
}

class PipelineCaptureReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val tag = intent.getStringExtra("tag") ?: "capture"
        if (!tag.matches(Regex("[a-zA-Z0-9_-]{1,40}"))) return
        runCatching { CameraDiagnostics.capture(context, tag, intent.getBooleanExtra("raw", false)) }
            .onFailure { Logger.log("CAPTURE", "Capture failed: ${it.message}") }
    }
}

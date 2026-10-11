package com.prismforge.studio

import android.graphics.Rect
import android.util.Log
import androidx.annotation.OptIn
import androidx.camera.core.ExperimentalGetImage
import androidx.camera.core.ImageProxy
import com.google.mlkit.vision.barcode.BarcodeScanner
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage

class QRScanner() {

    data class Result(
        var address: String,
        var port: Int
    )

    private var options: BarcodeScannerOptions
    private var scanner: BarcodeScanner

    private var addressRegex = Regex("""\b((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\b""")

    @Volatile
    private var enabled = false

    init {
        options = BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build()
        scanner = BarcodeScanning.getClient(options)
    }

    fun start() {
        enabled = true
    }

    fun stop() {
        enabled = false
    }

    @OptIn(ExperimentalGetImage::class)
    fun launchScanTask(imageProxy: ImageProxy, rect: Rect, callback: (QRScanner.Result?) -> Unit) {
        if (!enabled) {
            imageProxy.close()
            return
        }

        val mediaImage = imageProxy.image
        if (mediaImage != null) {
            val image = InputImage.fromMediaImage(mediaImage, imageProxy.imageInfo.rotationDegrees)
            scanner.process(image)
                .addOnSuccessListener { barcodes ->
                    if (!enabled) {
                        imageProxy.close()
                        return@addOnSuccessListener
                    }

                    for (barcode in barcodes) {
                        val parsed = parseResult(barcode.rawValue ?: "")
                        if (parsed != null) {
                            stop()
                            callback(parsed)
                            break
                        }
                    }
                    imageProxy.close()
                }
                .addOnFailureListener {
                    imageProxy.close()
                }
        } else {
            imageProxy.close()
        }
    }

    fun parseResult(value: String): Result? {
        val trimmed = value.trim()
        val splits = trimmed.split(":")
        if (splits.size != 2) {
            return null
        }
        val ip = splits[0].trim()
        val port = splits[1].trim().toIntOrNull() ?: return null
        if (ip.matches(addressRegex)) {
            return Result(ip, port)
        }
        return null
    }
}
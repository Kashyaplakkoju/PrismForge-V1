package com.prismforge.studio.video

/** The five-int RootEncoder overload ends in rotation, NOT keyframe interval. */
object VideoPreparation {
    const val KEYFRAME_INTERVAL_SECONDS = 2

    fun prepare(
        width: Int, height: Int, fps: Int, bitrate: Int, rotationDegrees: Int,
        backend: (Int, Int, Int, Int, Int, Int) -> Boolean
    ): Boolean {
        // Reject invalid angles instead of snapping a programming error (such as rotation=2).
        if (rotationDegrees !in 0..270 || rotationDegrees % 90 != 0) return false
        return backend(width, height, fps, bitrate, KEYFRAME_INTERVAL_SECONDS, rotationDegrees)
    }
}

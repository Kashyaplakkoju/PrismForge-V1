package com.prismforge.studio.video

/** Cardinal clockwise rotation for RootEncoder's camera render (including front mirror compensation). */
object RotationMath {
    fun resolve(
        sensorOrientation: Int,
        displayRotation: Int = 0,
        frontFacing: Boolean = false,
        rawOrientation: Int? = null
    ): Int {
        require(displayRotation in 0..3) { "Expected a Surface.ROTATION_* value" }
        // OrientationEventListener reports clockwise device angles; Display reports counterclockwise.
        val displayDegrees = if (rawOrientation != null && rawOrientation >= 0) {
            -(((rawOrientation.toLong() % 360 + 45) / 90 * 90) % 360)
        } else displayRotation * 90L
        val sensorDegrees = if (frontFacing) -sensorOrientation.toLong() else sensorOrientation.toLong()
        val normalized = ((sensorDegrees - displayDegrees) % 360 + 360) % 360
        return (((normalized + 45) / 90 * 90) % 360).toInt()
    }
}

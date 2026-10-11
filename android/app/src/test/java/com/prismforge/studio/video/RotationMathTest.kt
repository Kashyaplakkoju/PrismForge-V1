package com.prismforge.studio.video

import org.junit.Assert.*
import org.junit.Test

class RotationMathTest {
    @Test fun sensorDisplayAndLensCombinations() {
        val expected = mapOf(
            (90 to false) to listOf(90, 0, 270, 180),
            (270 to false) to listOf(270, 180, 90, 0),
            (90 to true) to listOf(270, 180, 90, 0),
            (270 to true) to listOf(90, 0, 270, 180)
        )
        expected.forEach { (camera, rotations) ->
            rotations.forEachIndexed { display, rotation ->
                assertEquals(rotation, RotationMath.resolve(camera.first, display, camera.second))
            }
        }
    }

    @Test fun rawAnglesSnapBeforeUseAndUseTheOppositeConventionToDisplay() {
        val snapped = mapOf(7 to 0, 44 to 0, 46 to 90, 91 to 90, 178 to 180, 359 to 0)
        for (sensor in listOf(90, 270)) for (front in listOf(false, true)) {
            snapped.forEach { (raw, cardinal) ->
                assertEquals(RotationMath.resolve(sensor, (4 - cardinal / 90) % 4, front),
                    RotationMath.resolve(sensor, frontFacing = front, rawOrientation = raw))
            }
            for (raw in 0..359) assertEquals(0,
                RotationMath.resolve(sensor, frontFacing = front, rawOrientation = raw) % 90)
        }
    }

    @Test fun unknownOrientationUsesDisplayAndSignedCommandsWrap() {
        assertEquals(0, RotationMath.resolve(90, 1, rawOrientation = -1))
        assertEquals(270, RotationMath.resolve(-90))
        assertEquals(90, RotationMath.resolve(450))
        assertEquals(0, RotationMath.resolve(0))
    }
}

package com.prismforge.studio.video

import org.junit.Assert.*
import org.junit.Test

class VideoPreparationTest {
    @Test fun intervalAndRotationReachTheSixArgumentOverloadSeparately() {
        for (rotation in listOf(0, 90, 180, 270)) {
            var received: List<Int>? = null
            assertTrue(VideoPreparation.prepare(1280, 720, 30, 4096000, rotation) { w, h, fps, rate, interval, angle ->
                received = listOf(w, h, fps, rate, interval, angle)
                true
            })
            assertEquals(listOf(1280, 720, 30, 4096000, 2, rotation), received)
        }
    }

    @Test fun twoDegreeRegressionNeverReachesEncoder() {
        for (rotation in listOf(2, 7, 44, 46, 91, 178, 359, -90, 360)) {
            assertFalse(VideoPreparation.prepare(1280, 720, 30, 4096000, rotation) { _, _, _, _, _, _ ->
                fail("Invalid angle reached encoder")
                true
            })
        }
    }

    @Test fun encoderRejectionIsPropagated() {
        assertFalse(VideoPreparation.prepare(1280, 720, 30, 4096000, 90) { _, _, _, _, _, _ -> false })
    }
}

package com.prismforge.studio.networking

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

class ConnectionRetryTest {
    @Test fun lateUsbTunnelBecomesAvailableWithoutRestart() = runBlocking {
        var now = 0L; var attempts = 0
        val result = connectWithRetry(8_000, { true }, { it is IOException }, { now }, { now += it }) {
            if (++attempts < 4) throw IOException("Tunnel not ready")
            "connected"
        }
        assertEquals("connected", result); assertEquals(4, attempts); assertEquals(750L, now)
    }
    @Test fun wifiFailureDoesNotRetry() = runBlocking {
        var attempts = 0; var waits = 0
        try {
            connectWithRetry(0, { true }, { true }, { 0 }, { waits++ }) {
                attempts++; throw IOException("Unavailable")
            }
            fail("Expected failure")
        } catch (_: IOException) { }
        assertEquals(1, attempts); assertEquals(0, waits)
    }
    @Test fun persistentFailureStopsAtDeadlineWithOriginalError() = runBlocking {
        var now = 0L; var attempts = 0
        val error = IOException("Still unavailable")
        try {
            connectWithRetry(600, { true }, { true }, { now }, { now += it }) { attempts++; throw error }
            fail("Expected bounded failure")
        } catch (actual: IOException) { assertSame(error, actual) }
        assertEquals(600L, now); assertEquals(3, attempts)
    }
    @Test fun supersededRequestDoesNotOpenAnotherSocket() = runBlocking {
        var current = true; var attempts = 0
        try {
            connectWithRetry(8_000, { current }, { true }, { 0 }, { current = false }) {
                attempts++; throw IOException("Not ready")
            }
            fail("Expected cancellation")
        } catch (_: CancellationException) { }
        assertEquals(1, attempts)
    }
    @Test fun programmerErrorsAreNotRetried() = runBlocking {
        var attempts = 0
        try {
            connectWithRetry(8_000, { true }, { it is IOException }, { 0 }, { fail("No retry") }) {
                attempts++; throw IllegalArgumentException("Bad port")
            }
            fail("Expected failure")
        } catch (_: IllegalArgumentException) { }
        assertEquals(1, attempts)
    }
    @Test fun cancellationFromConnectorIsNeverRetried() = runBlocking {
        try {
            connectWithRetry(8_000, { true }, { true }, { 0 }, { fail("No retry") }) {
                throw CancellationException("Cancelled")
            }
            fail("Expected cancellation")
        } catch (_: CancellationException) { }
    }
}

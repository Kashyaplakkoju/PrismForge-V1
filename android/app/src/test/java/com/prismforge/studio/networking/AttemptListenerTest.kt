package com.prismforge.studio.networking

import org.junit.Assert.*
import org.junit.Test

class AttemptListenerTest {
    @Test fun queuedOldDisconnectCannotCloseReplacementConnection() {
        var generation = 1
        var replacementOpen = true
        val queued = mutableListOf<() -> Unit>()
        val old = AttemptListener({ generation == 1 }, { queued.add(it) }, { _, _ -> }, { replacementOpen = false })
        old.onDisconnected()
        generation = 2
        queued.forEach { it() }
        assertTrue(replacementOpen)
    }
    @Test fun queuedOldBytesCannotReachReplacementConsumer() {
        var generation = 1
        var delivered = 0
        val queued = mutableListOf<() -> Unit>()
        val old = AttemptListener({ generation == 1 }, { queued.add(it) }, { _, _ -> delivered++ }, {})
        old.onBytesReceived(byteArrayOf(1), 1)
        generation = 2
        old.onBytesReceived(byteArrayOf(2), 1)
        queued.forEach { it() }
        assertEquals(0, delivered)
    }
    @Test fun currentCallbackOwnsBytesAndDisconnectsOnlyOnce() {
        var disconnected = 0
        var delivered: ByteArray? = null
        val queued = mutableListOf<() -> Unit>()
        val listener = AttemptListener({ true }, { queued.add(it) }, { bytes, _ -> delivered = bytes }, { disconnected++ })
        val buffer = byteArrayOf(7, 8)
        listener.onBytesReceived(buffer, 1)
        buffer[0] = 99
        queued.removeAt(0).invoke()
        assertArrayEquals(byteArrayOf(7), delivered)
        listener.onDisconnected(); listener.onDisconnected()
        queued.forEach { it() }
        assertTrue(listener.ended.get()); assertEquals(1, disconnected)
    }
}

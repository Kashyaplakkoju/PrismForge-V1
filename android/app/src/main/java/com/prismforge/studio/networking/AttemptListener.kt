package com.prismforge.studio.networking

import com.prismforge.studio.networking.connection.Connection
import java.util.concurrent.atomic.AtomicBoolean

/** Check identity again when queued callbacks execute, not only when received. */
internal class AttemptListener(
    private val current: () -> Boolean,
    private val dispatch: (() -> Unit) -> Unit,
    private val bytesReceived: (ByteArray, Int) -> Unit,
    private val disconnected: () -> Unit
) : Connection.Listener {
    val ended = AtomicBoolean(false)
    override fun onBytesReceived(buffer: ByteArray, bytes: Int) {
        if (!current() || ended.get()) return
        val owned = buffer.copyOf(bytes)
        dispatch { if (current() && !ended.get()) bytesReceived(owned, owned.size) }
    }
    override fun onDisconnected() {
        if (ended.getAndSet(true) || !current()) return
        dispatch { if (current()) disconnected() }
    }
}

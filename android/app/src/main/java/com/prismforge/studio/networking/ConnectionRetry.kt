package com.prismforge.studio.networking

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay

/** Bounded retry for the brief gap between USB attachment and ADB tunnel readiness. */
internal suspend fun <T> connectWithRetry(
    windowMs: Long,
    current: () -> Boolean,
    retryable: (Exception) -> Boolean,
    clock: () -> Long = { System.nanoTime() / 1_000_000 },
    wait: suspend (Long) -> Unit = { delay(it) },
    connect: () -> T
): T {
    require(windowMs in 0..15_000)
    val began = clock()
    var lastFailure: Exception? = null
    while (true) {
        if (!current()) throw CancellationException("Connection attempt superseded")
        if (lastFailure != null && clock() - began >= windowMs) throw lastFailure
        try {
            return connect()
        } catch (failure: Exception) {
            if (failure is CancellationException || !retryable(failure)) throw failure
            lastFailure = failure
            val remaining = windowMs - (clock() - began)
            if (remaining <= 0) throw failure
            wait(minOf(250L, remaining))
        }
    }
}

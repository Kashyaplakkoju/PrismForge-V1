package com.prismforge.studio.networking

/** Immutable identity/address for the connection that opened the streaming screen. */
data class ConnectionSession(val attempt: Long, val localIpAddress: String)

object SessionHandoff {
    fun accept(expectedAttempt: Long, current: ConnectionSession?): ConnectionSession? =
        current?.takeIf { expectedAttempt > 0 && it.attempt == expectedAttempt }
}

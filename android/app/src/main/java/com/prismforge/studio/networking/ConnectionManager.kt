package com.prismforge.studio.networking

import android.util.Log
import com.prismforge.studio.networking.connection.Connection
import com.prismforge.studio.networking.connection.TCPConnection
import com.prismforge.studio.util.Logger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicLong

class ConnectionManager private constructor() {

    private val TAG = "PrismForge Studio"

    companion object {
        @Volatile private var instance: ConnectionManager? = null

        fun getInstance(): ConnectionManager {
            synchronized(this) {
                return instance ?: ConnectionManager().also { instance = it }
            }
        }

        fun getInstance(connectionStateCallback: ConnectionStateCallback): ConnectionManager {
            synchronized(this) {
                if(instance == null) {
                    instance = ConnectionManager()
                }
                instance!!.setConnectionStateCallback(connectionStateCallback)
                return instance!!
            }
        }
    }

    enum class Mode {
        USB,
        WIFI
    }

    interface ConnectionStateCallback {
        fun onConnectionSuccessful(connectionMode: Mode) { }
        fun onConnectionFailed(connectionMode: Mode) { }
        fun onDisconnected() { }
    }

    // Read on the main dispatcher, where socket ownership is published/removed.
    val session: ConnectionSession?
        get() = tcpConn?.takeIf { currentMode != null }?.let {
            ConnectionSession(connectionAttempt.get(), it.localIpAddress)
        }

    fun isCurrentSession(attempt: Long): Boolean =
        connectionAttempt.get() == attempt && tcpConn != null && currentMode != null

    fun attachSession(attempt: Long, callback: ConnectionStateCallback,
                      bytesCallback: (ByteArray, Int) -> Unit): Boolean {
        if (!isCurrentSession(attempt)) return false
        connectionStateCallback = callback
        onBytesReceivedCallback = bytesCallback
        return true
    }

    private var connectionStateCallback: ConnectionStateCallback? = null
    private var onBytesReceivedCallback: ((buffer: ByteArray, bytes: Int) -> Unit)? = null

    @Volatile var currentMode: Mode? = null
        private set
    @Volatile private var tcpConn: TCPConnection? = null
    private var connectionJob: Job? = null
    private val connectionAttempt = AtomicLong(0)

    private var streamingEnabled = false

    private fun setConnectionStateCallback(connectionStateCallback: ConnectionStateCallback) {
        this.connectionStateCallback = connectionStateCallback
    }

    /**
     * The bytesReceivedListener is called only for packets of type different than PacketType.ACTIVATION.
     * These packets are handled internally
     */
    fun setOnBytesReceivedCallback(onBytesReceivedCallback: ((buffer: ByteArray, bytes: Int) -> Unit)?) {
        this.onBytesReceivedCallback = onBytesReceivedCallback
    }

    /**
     * Connect in WIFI mode.
     */
    fun connect(ipAddress: String, port: Int) {
        startConnection(ipAddress, port, Mode.WIFI)
    }

    /** USB can be tapped while the desktop is still recreating its tunnels. */
    fun connect(port: Int) {
        startConnection("127.0.0.1", port, Mode.USB)
    }

    private fun startConnection(address: String, port: Int, mode: Mode) {
        val attempt = connectionAttempt.incrementAndGet()
        connectionJob?.cancel()
        val previous = tcpConn
        tcpConn = null
        currentMode = null
        CoroutineScope(Dispatchers.IO).launch { previous?.close() }
        connectionJob = CoroutineScope(Dispatchers.IO).launch {
            var opened: TCPConnection? = null
            try {
                opened = connectWithRetry(
                    windowMs = if (mode == Mode.USB) 8_000 else 0,
                    current = { isActive && connectionAttempt.get() == attempt },
                    retryable = { it is Connection.ConnectionFailedException }
                ) {
                    val listener = AttemptListener(
                        current = { connectionAttempt.get() == attempt },
                        dispatch = { action -> CoroutineScope(Dispatchers.Main).launch { action() } },
                        bytesReceived = { bytes, size -> onBytesReceivedCallback?.invoke(bytes, size) },
                        disconnected = {
                            val connection = tcpConn
                            tcpConn = null
                            currentMode = null
                            streamingEnabled = false
                            CoroutineScope(Dispatchers.IO).launch { connection?.close() }
                            connectionStateCallback?.onDisconnected()
                        }
                    )
                    val connection = TCPConnection(address, port, mode == Mode.USB, listener,
                        connectTimeoutMs = if (mode == Mode.USB) 1_000 else 5_000)
                    if (listener.ended.get()) {
                        connection.close()
                        throw Connection.ConnectionFailedException("$address:$port", "Closed during connection")
                    }
                    connection
                }
                withContext(Dispatchers.Main) {
                    if (!isActive || connectionAttempt.get() != attempt) return@withContext
                    tcpConn = opened
                    tcpConn?.startReceiving()
                    currentMode = mode
                    opened = null // Ownership transferred only by the current attempt.
                    connectionStateCallback?.onConnectionSuccessful(mode)
                }
            } catch (failure: Connection.ConnectionFailedException) {
                Logger.log("CONNECTION MANAGER", "" + failure.message)
                withContext(Dispatchers.Main) {
                    if (connectionAttempt.get() == attempt) connectionStateCallback?.onConnectionFailed(mode)
                }
            } finally {
                opened?.close()
            }
        }
    }

    fun disconnect(expectedAttempt: Long? = null) {
        // A destroyed activity from an old attempt must not close its replacement.
        if (expectedAttempt != null && connectionAttempt.get() != expectedAttempt) return
        connectionAttempt.incrementAndGet()
        connectionJob?.cancel()
        connectionJob = null
        onBytesReceivedCallback = null
        connectionStateCallback = null
        currentMode = null
        val connection = tcpConn
        tcpConn = null
        CoroutineScope(Dispatchers.IO).launch { connection?.close() }
    }

    fun sendDescriptor(descriptor: DeviceDescriptor, expectedAttempt: Long? = null) {
        if (expectedAttempt != null && !isCurrentSession(expectedAttempt)) return
        val connection = tcpConn
        CoroutineScope(Dispatchers.IO).launch { connection?.send(descriptor.serialize()) }
    }

    fun sendErrorReport(report: ErrorReport, expectedAttempt: Long? = null) {
        if (expectedAttempt != null && !isCurrentSession(expectedAttempt)) return
        val connection = tcpConn
        CoroutineScope(Dispatchers.IO).launch { connection?.send(report.serialize()) }
    }

}
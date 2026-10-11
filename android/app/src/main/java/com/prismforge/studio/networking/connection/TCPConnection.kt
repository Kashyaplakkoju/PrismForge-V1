package com.prismforge.studio.networking.connection

import android.util.Log
import com.prismforge.studio.util.Logger
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket
import com.prismforge.studio.networking.CommandDecoder
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Wrapper around a socket to manage the TCP connection
 * @param ipAddress Remote endpoint's IPv4 address
 * @param port Remote endpoint's port
 * @param listener The registered listener for callbacks
 */
class TCPConnection(
    ipAddress: String,
    port: Int,
    private val isOverAdb: Boolean,
    private val listener: Connection.Listener,
    connectTimeoutMs: Int = 5_000
) : Connection() {

    override val maxPacketSize = 65472

    private val socket = Socket()
    private var outputStream: OutputStream? = null
    private var inputStream: InputStream? = null

    private var thread: Thread
    private val running = AtomicBoolean(true)

    override val localIpAddress: String
        get() = socket.localAddress.hostAddress!!

    init {
        try {
            socket.connect(java.net.InetSocketAddress(ipAddress, port), connectTimeoutMs)

            outputStream = socket.getOutputStream()
            inputStream = socket.getInputStream()

            Logger.log("TCPConnection", "Connected to $ipAddress:$port")

            thread = Thread { startReceiveBytesLoop() }

        } catch (e: Exception) {
            runCatching { socket.close() }
            throw ConnectionFailedException("$ipAddress:$port", e.message ?: "")
        }
    }

    fun startReceiving() { thread.start() }

    override fun send(bytes: ByteArray) {
        socket.getOutputStream().write(bytes)
    }

    override fun send(bytes: ByteArray, size: Int) {
        socket.getOutputStream().write(bytes, 0, size)
    }

    private fun startReceiveBytesLoop() {
        val buf = ByteArray(4096)
        val decoder = CommandDecoder()
        while (running.get()) {
            try {
                val bytes = inputStream?.read(buf)
                // When connected over ADB stream end of file might be reached
                if(bytes == -1) {
                    if (running.getAndSet(false)) listener.onDisconnected()
                    break
                }
                if (bytes != null && bytes > 0) decoder.feed(buf.copyOf(bytes)).forEach { listener.onBytesReceived(it, it.size) }
            } catch (e: Exception) {
                Logger.log("TCPConnection", "Error while reading. Closing socket. ${e.message}")
                if (running.getAndSet(false)) listener.onDisconnected()
                break
            }
        }
    }

    override fun close() {
        running.set(false)
        socket.close()
        if (Thread.currentThread() != thread) thread.join(1500)
    }
}
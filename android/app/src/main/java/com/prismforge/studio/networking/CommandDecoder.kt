package com.prismforge.studio.networking

/** Decodes the existing wire format without assuming TCP read boundaries are packets. */
class CommandDecoder {
    private var pending = byteArrayOf()
    fun feed(bytes: ByteArray): List<ByteArray> {
        require(pending.size + bytes.size <= 65536) { "Command buffer limit exceeded" }
        pending += bytes
        val result = mutableListOf<ByteArray>()
        while (pending.isNotEmpty()) {
            val length = packetLength() ?: break
            if (pending.size < length) break
            result += pending.copyOfRange(0, length)
            pending = pending.copyOfRange(length, pending.size)
        }
        return result
    }
    private fun u16(offset: Int) = ((pending[offset].toInt() and 255) shl 8) or (pending[offset + 1].toInt() and 255)
    private fun packetLength(): Int? = when (pending[0]) {
        PacketType.CAMERA -> 1
        PacketType.ROTATION, PacketType.STABILIZATION, PacketType.FLASH, PacketType.FOCUS,
        PacketType.CODEC, PacketType.FPS, PacketType.FLIP -> 2
        PacketType.BITRATE -> 3
        PacketType.RESOLUTION, PacketType.ADAPTIVE_BITRATE, PacketType.ZOOM -> 5
        PacketType.EFFECT_FILTER, PacketType.CORRECTION_FILTER -> if (pending.size < 2) null
            else 2 + (pending[1].toInt() and 255) + if (pending[0] == PacketType.CORRECTION_FILTER) 1 else 0
        PacketType.ACTIVATION -> activationLength()
        else -> throw IllegalArgumentException("Unknown camera command")
    }
    private fun activationLength(): Int? {
        if (pending.size < 39) return null
        val count = u16(37)
        require(count <= 128) { "Too many filter settings" }
        var offset = 39
        repeat(count) {
            if (pending.size < offset + 2) return null
            val length = u16(offset)
            require(length <= 1024) { "Filter name too long" }
            offset += 2 + length + 4
            if (pending.size < offset) return null
        }
        if (pending.size < offset + 2) return null
        val length = u16(offset)
        require(length <= 1024) { "Effect name too long" }
        return offset + 2 + length
    }
}

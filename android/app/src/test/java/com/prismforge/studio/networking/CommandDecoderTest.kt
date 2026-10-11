package com.prismforge.studio.networking

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer

class CommandDecoderTest {
    @Test fun fragmentedAndCoalescedCommandsRetainBoundaries() {
        val decoder = CommandDecoder()
        assertTrue(decoder.feed(byteArrayOf(PacketType.RESOLUTION, 0x80.toByte())).isEmpty())
        val packets = decoder.feed(byteArrayOf(0x07, 0x38, 0x04, PacketType.CAMERA, PacketType.FPS, 30))
        assertEquals(3, packets.size)
        assertArrayEquals(byteArrayOf(PacketType.RESOLUTION, 0x80.toByte(), 0x07, 0x38, 0x04), packets[0])
        assertArrayEquals(byteArrayOf(PacketType.CAMERA), packets[1])
        assertArrayEquals(byteArrayOf(PacketType.FPS, 30), packets[2])
    }

    @Test fun activationWaitsForEntireProfileAtEverySplit() {
        val profile = ByteBuffer.allocate(60).apply {
            put(PacketType.ACTIVATION)
            putInt(30); putInt(1280); putInt(720); putInt(1); put(0)
            putInt(4000); putInt(500); putInt(8000)
            put(0); put(0); put(0); putInt(0)
            putShort(1); putShort(8); put("Contrast".toByteArray()); putInt(50)
            putShort(4); put("None".toByteArray())
        }.let { it.array().copyOf(it.position()) }
        for (split in 1 until profile.size) {
            val decoder = CommandDecoder()
            assertTrue("Split $split", decoder.feed(profile.copyOfRange(0, split)).isEmpty())
            val packets = decoder.feed(profile.copyOfRange(split, profile.size) + byteArrayOf(PacketType.CAMERA))
            assertEquals(2, packets.size)
            assertArrayEquals(profile, packets[0])
        }
    }

    @Test fun unsignedFilterNameLengthIsSupported() {
        val packet = byteArrayOf(PacketType.EFFECT_FILTER, 200.toByte()) + ByteArray(200) { 65 }
        val decoder = CommandDecoder()
        assertTrue(decoder.feed(packet.copyOfRange(0, 100)).isEmpty())
        assertArrayEquals(packet, decoder.feed(packet.copyOfRange(100, packet.size)).single())
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsUnknownCommands() { CommandDecoder().feed(byteArrayOf(127)) }

    @Test(expected = IllegalArgumentException::class)
    fun boundsPendingInput() { CommandDecoder().feed(ByteArray(65537)) }

    @Test(expected = IllegalArgumentException::class)
    fun boundsActivationFilterCount() {
        val packet = ByteArray(39); packet[0] = PacketType.ACTIVATION; packet[37] = 1
        CommandDecoder().feed(packet)
    }
}

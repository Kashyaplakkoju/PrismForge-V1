package com.prismforge.studio.networking

import org.junit.Assert.*
import org.junit.Test

class SessionHandoffTest {
    @Test fun successThenEofBeforeScreenCreationRejectsLaunch() {
        val opened = ConnectionSession(4, "127.0.0.1")
        assertEquals(opened, SessionHandoff.accept(4, opened))
        assertNull(SessionHandoff.accept(4, null))
    }
    @Test fun oldScreenCannotAttachToReplacementSession() {
        val replacement = ConnectionSession(5, "192.168.1.5")
        assertNull(SessionHandoff.accept(4, replacement))
        assertSame(replacement, SessionHandoff.accept(5, replacement))
    }
    @Test fun acceptedAddressIsAnImmutableSnapshot() {
        val opened = ConnectionSession(4, "127.0.0.1")
        val accepted = SessionHandoff.accept(4, opened)!!
        assertEquals("127.0.0.1", accepted.localIpAddress)
        assertNull(SessionHandoff.accept(4, ConnectionSession(5, "192.168.1.5")))
        assertEquals("127.0.0.1", accepted.localIpAddress)
    }
    @Test fun absentIntentIdentityCannotAttachToAnySession() {
        assertNull(SessionHandoff.accept(-1, ConnectionSession(5, "127.0.0.1")))
    }
}

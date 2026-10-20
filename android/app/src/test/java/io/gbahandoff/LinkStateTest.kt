package io.gbahandoff

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.TimeZone

class LinkStateTest {
    private val a = "a".repeat(40)
    private val b = "0123456789abcdef0123456789abcdef01234567"

    @Test fun emptyAndNull() {
        assertEquals(LinkState(), LinkState.parse(null))
        assertEquals(LinkState(), LinkState.parse(""))
    }

    @Test fun fullSnapshot() {
        val text = """
            S 2 MEU PC do Lucas
            H $a 1700000200000
            W $b
            P 1 $a
            O $b
            T $a 42
            C $a 1700000000000 3600 0 1700000100000 7200 1 1 0 1
            Q $b 1
            D 1 0
            N Jogando no PC
            N Cabo desconectado: retomando
        """.trimIndent() + "\n"
        val s = LinkState.parse(text)
        assertEquals(LinkClient.CONNECTED, s.status)
        assertEquals("MEU PC do Lucas", s.peer)
        assertEquals(setOf(a), s.onPc)
        assertEquals(mapOf(a to 1700000200000L), s.onPcTs)
        assertEquals(mapOf(b to true), s.places)
        assertEquals(Pair(true, false), s.settingsDiff)
        assertEquals(setOf(b), s.waiting)
        assertEquals(a, s.pcPlaying)
        assertEquals(b, s.openRequest)
        assertEquals(mapOf(a to 42), s.receiving)
        assertEquals(listOf("Jogando no PC", "Cabo desconectado: retomando"), s.notices)
        val c = s.conflicts.single()
        assertEquals(Conflict(a, 1700000000000, 3600, 0, 1700000100000, 7200, 1, true, false, true), c)
    }

    @Test fun refusalAndOldFormats() {
        val s = LinkState.parse("S 3 \nX 1 2\nH $a\nQ $b 0\n")
        assertEquals(LinkClient.REFUSED, s.status)
        assertEquals(1, s.refusalStatus); assertEquals(2, s.refusalProto)
        assertEquals(mapOf(a to 0L), s.onPcTs)           // H without a time still parses
        assertEquals(mapOf(b to false), s.places)
        assertNull(LinkState.parse("D 1 1\n".replace("1 1", "x y")).settingsDiff)
    }

    @Test fun pcNotPlayingAndEmptyPeer() {
        val s = LinkState.parse("S 1 \nP 0 -\n")
        assertEquals(LinkClient.SEARCHING, s.status)
        assertEquals("", s.peer)
        assertNull(s.pcPlaying)
    }

    @Test fun malformedRecordsAreSkipped() {
        val s = LinkState.parse("X nada\nQ zz 1\nD 1\nH curto\nC $a 1 2\nC $a 1 2 3 4 5 6 7 x 9\nT $a\nS notnum\nO zz\nN \n\n??\n")
        assertEquals(LinkState(), s)
    }

    @Test fun percentIsClamped() {
        assertEquals(100, LinkState.parse("T $a 999\n").receiving[a])
        assertEquals(0, LinkState.parse("T $a -5\n").receiving[a])
    }

    @Test fun formatting() {
        val utc = TimeZone.getTimeZone("UTC")
        assertEquals("14/11 22:13", Fmt.dateTime(1700000000000, utc))
        assertEquals("—", Fmt.dateTime(0, utc))
        assertEquals("0s", Fmt.playTime(0))
        assertEquals("1s", Fmt.playTime(60))
        assertEquals("1m 00s", Fmt.playTime(3600))
        assertEquals("1h 00m", Fmt.playTime((3600 * Fmt.GBA_FPS).toLong() + 5))
        assertEquals("Celular", Fmt.device(0))
        assertEquals("PC", Fmt.device(1))
        assertEquals("Outro aparelho", Fmt.device(2))
        assertFalse(Fmt.shortSha(a).length != 8)
        assertTrue(Fmt.shortSha(b).startsWith("01234567"))
    }
}

package io.gbahandoff

import org.junit.Assert.*
import org.junit.Test

class PadLayoutTest {
    private class MapKv : KvStore {
        val m = HashMap<String, String>()
        override fun get(key: String) = m[key]
        override fun put(key: String, value: String) { m[key] = value }
    }

    private fun inside(l: PadLayout, w: Float, h: Float) {
        for (i in 0 until PadIds.COUNT) {
            val x = l.centerXpx(i, w); val y = l.centerYpx(i, h)
            val hw = l.halfW(i, w, h); val hh = l.halfH(i, w, h)
            assertTrue("$i left", x - hw >= -0.01f); assertTrue("$i right", x + hw <= w + 0.01f)
            assertTrue("$i top", y - hh >= -0.01f); assertTrue("$i bottom", y + hh <= h + 0.01f)
        }
    }

    @Test fun roundTrip() {
        val l = PadLayout(false)
        l.cx[3] = 0.31f; l.cy[3] = 0.42f; l.scale[3] = 1.7f; l.alpha[3] = 0.8f
        val r = PadLayout.from(false, l.encode())
        assertArrayEquals(l.cx, r.cx, 0f); assertArrayEquals(l.cy, r.cy, 0f)
        assertArrayEquals(l.scale, r.scale, 0f); assertArrayEquals(l.alpha, r.alpha, 0f)
    }

    @Test fun garbageFallsBackToDefaults() {
        val def = PadLayout(true)
        for (s in listOf(null, "", "xx", "1,2,3", "9,0.5,0.5,1,1", "0,NaN,0.5,1,1", "0,a,b,c,d", ";;;")) {
            val r = PadLayout.from(true, s)
            assertArrayEquals(s, def.cx, r.cx, 0f); assertArrayEquals(s, def.alpha, r.alpha, 0f)
        }
    }

    @Test fun decodeClampsRanges() {
        val r = PadLayout.from(false, "0,5,-3,99,-1;1,0.5,0.5,1,1")
        assertEquals(1f, r.cx[0], 0f); assertEquals(0f, r.cy[0], 0f)
        assertEquals(PadLayout.MAX_SCALE, r.scale[0], 0f); assertEquals(PadLayout.MIN_ALPHA, r.alpha[0], 0f)
        assertEquals(0.5f, r.cx[1], 0f)                        // good record still applied
        assertEquals(PadLayout(false).cx[2], r.cx[2], 0f)      // missing record -> default
    }

    @Test fun moveClampsInsideScreen() {
        val w = 1080f; val h = 2400f
        val l = PadLayout(false)
        for (i in 0 until PadIds.COUNT) {
            l.moveTo(i, -500f, -500f, w, h); inside(l, w, h)
            l.moveTo(i, 9999f, 9999f, w, h); inside(l, w, h)
        }
        l.moveTo(PadIds.A, 5000f, 5000f, w, h)
        assertEquals(w - l.halfW(PadIds.A, w, h), l.centerXpx(PadIds.A, w), 0.01f)
    }

    @Test fun growingAtEdgeStaysInside() {
        val w = 2400f; val h = 1080f
        val l = PadLayout(true)
        l.moveTo(PadIds.R, w, 0f, w, h)
        l.setScale(PadIds.R, 2f, w, h); inside(l, w, h)
        l.setScale(PadIds.DPAD, 99f, w, h)
        assertEquals(PadLayout.MAX_SCALE, l.scale[PadIds.DPAD], 0f); inside(l, w, h)
    }

    @Test fun relativeToPixelsAndBackAcrossScreens() {
        val l = PadLayout(false)
        l.moveTo(PadIds.B, 540f, 1200f, 1080f, 2400f)
        assertEquals(0.5f, l.cx[PadIds.B], 1e-6f); assertEquals(0.5f, l.cy[PadIds.B], 1e-6f)
        assertEquals(360f, l.centerXpx(PadIds.B, 720f), 1e-3f)   // another screen: same fraction
        assertEquals(800f, l.centerYpx(PadIds.B, 1600f), 1e-3f)
    }

    @Test fun smallScreenKeepsEverythingInside() {
        for ((w, h) in listOf(480f to 800f, 800f to 480f, 1080f to 2400f, 2400f to 1080f)) {
            val l = PadLayout(w > h); l.clampAll(w, h); inside(l, w, h)
        }
    }

    @Test fun storeKeepsOrientationsSeparate() {
        val kv = MapKv(); val s = PadLayoutStore(kv)
        val p = s.load(false); p.cx[PadIds.A] = 0.3f; p.scale[PadIds.A] = 1.5f; s.save(p)
        val land = s.load(true)
        assertEquals(PadLayout(true).cx[PadIds.A], land.cx[PadIds.A], 0f)     // landscape untouched
        land.alpha[PadIds.DPAD] = 0.9f; s.save(land)
        assertEquals(0.3f, PadLayoutStore(kv).load(false).cx[PadIds.A], 0f)   // survives a "new process"
        assertEquals(1.5f, s.load(false).scale[PadIds.A], 0f)
        assertEquals(PadLayout(false).alpha[PadIds.DPAD], s.load(false).alpha[PadIds.DPAD], 0f)
        assertEquals(0.9f, s.load(true).alpha[PadIds.DPAD], 0f)
        assertEquals(2, kv.m.size)
    }

    @Test fun resetRestoresDefaults() {
        val l = PadLayout(false); l.cx[0] = 0.9f; l.scale[0] = 2f; l.setAllAlpha(1f)
        l.reset()
        assertArrayEquals(PadLayout(false).encode().toByteArray(), l.encode().toByteArray())
    }

    @Test fun newDefaultsAreSmallerAndMoreTransparent() {
        // Old D-pad: 3 * min(w/6.5, 72dp) in portrait. 1080px wide @ 2.75 density.
        val oldDpad = 3f * minOf(1080f / 6.5f, 72 * 2.75f)
        val l = PadLayout(false)
        val now = l.halfW(PadIds.DPAD, 1080f, 2400f) * 2
        assertTrue("$now vs $oldDpad", now <= oldDpad * 0.7f)
        assertEquals(0.5f, l.alpha[0], 0f)
    }

    @Test fun setAlphaClamps() {
        val l = PadLayout(false); l.setAlpha(1, 5f); assertEquals(1f, l.alpha[1], 0f)
        l.setAlpha(1, -1f); assertEquals(PadLayout.MIN_ALPHA, l.alpha[1], 0f)
    }
}

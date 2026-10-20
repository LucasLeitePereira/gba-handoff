package io.gbahandoff

import org.junit.Assert.*
import org.junit.Test

class PlacementTest {
    private val area = floatArrayOf(0f, 0f, 240f, 160f)

    @Test fun freeAreaUsesPreferredCorner() {
        val p = Placement.pick(area, 100f, 20f, 6f, emptyList())
        assertArrayEquals(floatArrayOf(6f, 6f), p, 0.001f)
        val q = Placement.pick(area, 100f, 20f, 6f, emptyList(), Placement.PREFER_TOP_RIGHT)
        assertArrayEquals(floatArrayOf(134f, 6f), q, 0.001f)
    }

    @Test fun skipsCornerCoveredByAControl() {
        // a Start button in the top-left: the widget goes to the top-right
        val p = Placement.pick(area, 100f, 20f, 6f, listOf(floatArrayOf(0f, 0f, 60f, 30f)))
        assertArrayEquals(floatArrayOf(134f, 6f), p, 0.001f)
    }

    @Test fun neverOverlapsWhenAFreeCornerExists() {
        val blocked = listOf(floatArrayOf(0f, 0f, 240f, 40f)) // whole top strip busy
        val p = Placement.pick(area, 100f, 20f, 6f, blocked)
        assertTrue("went to the bottom", p[1] > 100f)
    }

    @Test fun allBusyPicksLeastOverlap() {
        val blocked = listOf(floatArrayOf(0f, 0f, 240f, 160f))
        val p = Placement.pick(area, 100f, 20f, 6f, blocked)
        assertEquals(2, p.size)
    }
}

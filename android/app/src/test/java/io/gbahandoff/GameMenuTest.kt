package io.gbahandoff

import org.junit.Assert.*
import org.junit.Test

class GameMenuTest {
    private val d = 2.75f
    private var size = GameMenuLogic.BUTTON_DP * d

    private fun overlaps(b: FloatArray, p: FloatArray): Boolean =
        minOf(b[2], p[0] + size) - maxOf(b[0], p[0]) > 0f && minOf(b[3], p[1] + size) - maxOf(b[1], p[1]) > 0f

    private fun check(w: Float, h: Float, land: Boolean, d: Float = this.d, mutate: (PadLayout) -> Unit = {}) {
        size = GameMenuLogic.BUTTON_DP * d
        val layout = PadLayout(land)
        mutate(layout)
        layout.clampAll(w, h)
        val blocked = ArrayList<FloatArray>()
        layout.occupiedRects(w, h, 8 * d, blocked)
        val p = GameMenuLogic.buttonPos(w, h, d, blocked)
        for (b in blocked) assertFalse("button over a control ${b.toList()} at ${p.toList()}", overlaps(b, p))
        assertTrue("inside the screen", p[0] >= 0 && p[1] >= 0 && p[0] + size <= w && p[1] + size <= h)
    }

    @Test fun defaultsPortraitAndLandscape() {
        check(1080f, 2400f, false)
        check(2400f, 1080f, true)
        check(720f, 1280f, false, 2f)
        check(1280f, 720f, true, 2f)   // a 720p phone has density ~2, not 2.75
    }

    @Test fun avoidsAControlDraggedIntoTheTopRightCorner() {
        val w = 2400f; val h = 1080f
        val blocked = ArrayList<FloatArray>()
        val l = PadLayout(true)
        l.moveTo(PadIds.A, w - 10f, 10f, w, h)   // A parked in the preferred corner
        l.occupiedRects(w, h, 8 * d, blocked)
        val p = GameMenuLogic.buttonPos(w, h, d, blocked)
        assertTrue("moved away from the top-right", p[0] < w / 2 || p[1] > h / 2)
        size = GameMenuLogic.BUTTON_DP * d
        for (b in blocked) assertFalse(overlaps(b, p))
        // control: with nothing there it takes the top right
        val free = GameMenuLogic.buttonPos(w, h, d, emptyList())
        assertEquals(w - 6f * d - size, free[0], 0.01f)
        assertEquals(6f * d, free[1], 0.01f)
    }

    @Test fun differentOrientationsAreIndependent() {
        // the same blocked rects in one orientation must not constrain the other: each call is pure
        val a = GameMenuLogic.buttonPos(1080f, 2400f, d, emptyList())
        val b = GameMenuLogic.buttonPos(2400f, 1080f, d, emptyList())
        assertEquals(1080f - 6f * d - size, a[0], 0.01f)
        assertEquals(2400f - 6f * d - size, b[0], 0.01f)
    }

    @Test fun itemsWhenGameIsOnThePhone() {
        assertEquals(listOf(GameMenuLogic.Item.EDIT, GameMenuLogic.Item.SETTINGS, GameMenuLogic.Item.SEND, GameMenuLogic.Item.EXIT),
            GameMenuLogic.items(connected = true, here = false, control = false))
        assertFalse(GameMenuLogic.Item.SEND in GameMenuLogic.items(false, false, false))
    }

    @Test fun itemsWhenGameIsOnThePc() {
        val control = GameMenuLogic.items(true, true, true)
        assertTrue(GameMenuLogic.Item.EDIT in control && GameMenuLogic.Item.SETTINGS in control)
        assertFalse(GameMenuLogic.Item.SEND in control)
        val plain = GameMenuLogic.items(true, true, false)
        assertFalse("pad hidden: nothing to edit", GameMenuLogic.Item.EDIT in plain)
        assertTrue(GameMenuLogic.Item.SETTINGS in plain && GameMenuLogic.Item.EXIT in plain)
    }

    @Test fun occupiedRectsMatchTheControls() {
        val l = PadLayout(false)
        val out = ArrayList<FloatArray>()
        l.occupiedRects(1000f, 2000f, 10f, out)
        assertEquals(PadIds.COUNT, out.size)
        val a = out[PadIds.A]
        assertEquals(l.centerXpx(PadIds.A, 1000f) - l.halfW(PadIds.A, 1000f, 2000f) - 10f, a[0], 0.01f)
    }
}

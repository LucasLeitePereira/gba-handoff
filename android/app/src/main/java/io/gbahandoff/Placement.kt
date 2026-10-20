package io.gbahandoff

/**
 * Pure geometry (no android.*, unit-testable): where to put a small floating widget inside an area
 * so that it does not cover pad controls. Rects are [left, top, right, bottom] in the same space.
 */
object Placement {
    const val TOP_LEFT = 0; const val TOP_RIGHT = 1; const val BOTTOM_LEFT = 2; const val BOTTOM_RIGHT = 3

    /** Corner order for [pick]; each preference lists corners from the most to the least wanted. */
    val PREFER_TOP_LEFT = intArrayOf(TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT)
    val PREFER_TOP_RIGHT = intArrayOf(TOP_RIGHT, TOP_LEFT, BOTTOM_RIGHT, BOTTOM_LEFT)

    private fun overlap(a: FloatArray, l: Float, t: Float, r: Float, b: Float): Float {
        val w = minOf(a[2], r) - maxOf(a[0], l)
        val h = minOf(a[3], b) - maxOf(a[1], t)
        return if (w > 0f && h > 0f) w * h else 0f
    }

    /**
     * Picks the first corner of [area] (inset by [margin]) where a [w] x [h] widget touches none of
     * [blocked]; if every corner collides, the one with the least overlapping area. Returns
     * floatArrayOf(left, top) of the widget.
     */
    fun pick(area: FloatArray, w: Float, h: Float, margin: Float, blocked: List<FloatArray>, order: IntArray = PREFER_TOP_LEFT): FloatArray {
        var best = floatArrayOf(area[0] + margin, area[1] + margin)
        var bestCost = Float.MAX_VALUE
        for (c in order) {
            val l = if (c == TOP_LEFT || c == BOTTOM_LEFT) area[0] + margin else area[2] - margin - w
            val t = if (c == TOP_LEFT || c == TOP_RIGHT) area[1] + margin else area[3] - margin - h
            var cost = 0f
            for (b in blocked) cost += overlap(b, l, t, l + w, t + h)
            if (cost == 0f) return floatArrayOf(l, t)
            if (cost < bestCost) { bestCost = cost; best = floatArrayOf(l, t) }
        }
        return best
    }
}

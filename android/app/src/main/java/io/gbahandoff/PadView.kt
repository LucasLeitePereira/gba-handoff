package io.gbahandoff

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View

/**
 * Virtual gamepad: D-pad, A, B, L, R, Start, Select, laid out by [PadLayout] (per orientation,
 * persisted by [PadLayoutStore]). Play mode: multitouch, every event recomputes the pressed set from
 * ALL active pointers. Edit mode: drag a control to move it, tap to select it.
 * Entry to edit mode: LONG-PRESS (0.7 s) on the small gear at the top-center (a plain tap does
 * nothing, so it cannot be hit by accident mid-game).
 * No allocation in onTouchEvent/onDraw (rects/paints are preallocated).
 */
class PadView(
    context: Context,
    private val store: PadLayoutStore,
    private val onKeys: (Int) -> Unit,
) : View(context) {
    private val n = PadIds.COUNT
    private val rects = Array(n) { RectF() }
    private val keyOf = intArrayOf(0, Native.KEY_A, Native.KEY_B, Native.KEY_L, Native.KEY_R, Native.KEY_START, Native.KEY_SELECT)
    private val labels = arrayOf("", "A", "B", "L", "R", "START", "SELECT")

    private var layout = PadLayout(false)
    private var vw = 0f
    private var vh = 0f
    private var dcx = 0f; private var dcy = 0f; private var dead = 0f

    private val density = resources.displayMetrics.density
    private val white = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt() }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF202020.toInt(); textAlign = Paint.Align.CENTER }
    private val outline = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2 * density }
    private val gearPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 2 * density; color = 0x66FFFFFF; strokeCap = Paint.Cap.ROUND
    }
    private val gear = RectF()
    private val gearLines = FloatArray(6 * 4)
    private var keys = 0

    // Edit state.
    var editing = false
        set(v) {
            if (field == v) return
            field = v
            if (v) { setKeysNow(0); cancelGear() } else selected = -1
            drag = -1
            invalidate()
        }
    var selected = -1
        private set
    var onSelectionChanged: (() -> Unit)? = null
    var onGearLongPress: (() -> Unit)? = null
    private var drag = -1
    private var dragPointer = -1
    private var dragDx = 0f; private var dragDy = 0f
    private var gearDown = false
    private val gearRun = Runnable {
        if (gearDown) {
            gearDown = false
            performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
            onGearLongPress?.invoke()
        }
    }

    fun selectedAlpha() = if (selected >= 0) layout.alpha[selected] else layout.alpha[0]
    fun selectedScale() = if (selected >= 0) layout.scale[selected] else 1f

    fun setSelectedScale(s: Float) {
        if (selected < 0) return
        layout.setScale(selected, s, vw, vh); relayout(); invalidate()
    }
    fun setSelectedAlpha(a: Float) { if (selected >= 0) { layout.setAlpha(selected, a); invalidate() } }
    fun setAllAlpha(a: Float) { layout.setAllAlpha(a); invalidate() }
    fun resetLayout() { layout.reset(); layout.clampAll(vw, vh); relayout(); save(); invalidate() }
    fun save() = store.save(layout)

    private fun setKeysNow(k: Int) { if (k != keys) { keys = k; onKeys(k); invalidate() } }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        vw = w.toFloat(); vh = h.toFloat()
        if (w == 0 || h == 0) return
        val land = w > h
        if (land != layout.land || ow == 0 && oh == 0) layout = store.load(land)
        layout.clampAll(vw, vh)
        relayout()
        val g = 36 * density
        gear.set(vw / 2 - g / 2, 4 * density, vw / 2 + g / 2, 4 * density + g)
        // 6 teeth: radial ticks around the gear's ring.
        val cx = gear.centerX(); val cy = gear.centerY(); val r0 = g * 0.28f; val r1 = g * 0.42f
        for (i in 0 until 6) {
            val a = i * (Math.PI / 3).toFloat()
            val ca = Math.cos(a.toDouble()).toFloat(); val sa = Math.sin(a.toDouble()).toFloat()
            gearLines[i * 4] = cx + ca * r0; gearLines[i * 4 + 1] = cy + sa * r0
            gearLines[i * 4 + 2] = cx + ca * r1; gearLines[i * 4 + 3] = cy + sa * r1
        }
    }

    private fun relayout() {
        for (i in 0 until n) {
            val x = layout.centerXpx(i, vw); val y = layout.centerYpx(i, vh)
            val hw = layout.halfW(i, vw, vh); val hh = layout.halfH(i, vw, vh)
            rects[i].set(x - hw, y - hh, x + hw, y + hh)
        }
        val d = rects[PadIds.DPAD]
        dcx = d.centerX(); dcy = d.centerY(); dead = d.width() * 0.14f
    }

    private fun hit(rc: RectF, x: Float, y: Float): Boolean {
        // Touch targets are inflated a bit: thumbs are imprecise.
        val s = 8 * density
        return x >= rc.left - s && x <= rc.right + s && y >= rc.top - s && y <= rc.bottom + s
    }

    private fun inGear(x: Float, y: Float): Boolean {
        val s = 6 * density
        return x >= gear.left - s && x <= gear.right + s && y >= gear.top - s && y <= gear.bottom + s
    }

    private fun cancelGear() { gearDown = false; removeCallbacks(gearRun) }

    override fun onTouchEvent(e: MotionEvent): Boolean =
        if (editing) editTouch(e) else playTouch(e)

    private fun playTouch(e: MotionEvent): Boolean {
        var k = 0
        val act = e.actionMasked
        if (act == MotionEvent.ACTION_DOWN && inGear(e.x, e.y)) {
            gearDown = true
            postDelayed(gearRun, 700)
        } else if (gearDown && (act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_CANCEL ||
                act == MotionEvent.ACTION_POINTER_DOWN || (act == MotionEvent.ACTION_MOVE && !inGear(e.x, e.y)))) {
            cancelGear()
        }
        if (act != MotionEvent.ACTION_UP && act != MotionEvent.ACTION_CANCEL) {
            val skip = if (act == MotionEvent.ACTION_POINTER_UP) e.actionIndex else -1
            val dp = rects[PadIds.DPAD]
            // D-pad region is generous (1.25x) so sliding off the edge doesn't drop the direction.
            val pad = dp.width() * 0.125f
            for (i in 0 until e.pointerCount) {
                if (i == skip) continue
                val x = e.getX(i); val y = e.getY(i)
                for (b in 1 until n) if (hit(rects[b], x, y)) k = k or keyOf[b]
                if (x >= dp.left - pad && x <= dp.right + pad && y >= dp.top - pad && y <= dp.bottom + pad) {
                    val dx = x - dcx; val dy = y - dcy
                    if (dx > dead) k = k or Native.KEY_RIGHT else if (dx < -dead) k = k or Native.KEY_LEFT
                    if (dy > dead) k = k or Native.KEY_DOWN else if (dy < -dead) k = k or Native.KEY_UP
                }
            }
        }
        setKeysNow(k)
        return true
    }

    private fun select(i: Int) {
        if (i != selected) { selected = i; onSelectionChanged?.invoke(); invalidate() }
    }

    private fun editTouch(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // Smallest control under the finger wins (so a small button on top of the D-pad is pickable).
                var best = -1; var bestArea = Float.MAX_VALUE
                for (i in 0 until n) {
                    val rc = rects[i]
                    if (hit(rc, e.x, e.y)) {
                        val a = rc.width() * rc.height()
                        if (a < bestArea) { bestArea = a; best = i }
                    }
                }
                select(best)
                drag = best
                if (best >= 0) {
                    dragPointer = e.getPointerId(0)
                    dragDx = rects[best].centerX() - e.x; dragDy = rects[best].centerY() - e.y
                }
            }
            MotionEvent.ACTION_MOVE -> if (drag >= 0) {
                val pi = e.findPointerIndex(dragPointer)
                if (pi >= 0) {
                    layout.moveTo(drag, e.getX(pi) + dragDx, e.getY(pi) + dragDy, vw, vh)
                    relayout(); invalidate()
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (drag >= 0) save()
                drag = -1
            }
        }
        return true
    }

    override fun onDraw(c: Canvas) {
        for (i in 0 until n) {
            val a = layout.alpha[i]
            var idle = a * 0.5f
            if (editing && idle < 0.3f) idle = 0.3f   // keep faint controls findable while editing
            val on = i != PadIds.DPAD && keys and keyOf[i] != 0
            val fillA = if (on) minOf(1f, idle + 0.3f) else idle
            val rc = rects[i]
            if (i == PadIds.DPAD) drawDpad(c, rc, idle, minOf(1f, idle + 0.3f))
            else {
                white.alpha = (fillA * 255).toInt()
                val bw = rc.width(); val bh = rc.height()
                if (i <= PadIds.B) c.drawOval(rc, white) else c.drawRoundRect(rc, bh * 0.3f, bh * 0.3f, white)
                text.alpha = (minOf(1f, fillA + 0.35f) * 255).toInt()
                text.textSize = if (i <= PadIds.B) bw * 0.4f else minOf(bh * 0.5f, bw / 4.2f)
                c.drawText(labels[i], rc.centerX(), rc.centerY() + text.textSize * 0.35f, text)
            }
            if (editing) {
                outline.color = if (i == selected) 0xFFFFD54F.toInt() else 0x99FFFFFF.toInt()
                c.drawRect(rc, outline)
            }
        }
        if (!editing) {
            c.drawCircle(gear.centerX(), gear.centerY(), gear.width() * 0.2f, gearPaint)
            c.drawLines(gearLines, gearPaint)
        }
    }

    private fun drawDpad(c: Canvas, d: RectF, idle: Float, pressed: Float) {
        val t = d.width() / 3f
        white.alpha = (idle * 255).toInt()
        c.drawRoundRect(dcx - t / 2, d.top, dcx + t / 2, d.bottom, t / 4, t / 4, white)
        c.drawRoundRect(d.left, dcy - t / 2, d.right, dcy + t / 2, t / 4, t / 4, white)
        white.alpha = (pressed * 255).toInt()
        if (keys and Native.KEY_UP != 0) c.drawRect(dcx - t / 2, d.top, dcx + t / 2, dcy - t / 2, white)
        if (keys and Native.KEY_DOWN != 0) c.drawRect(dcx - t / 2, dcy + t / 2, dcx + t / 2, d.bottom, white)
        if (keys and Native.KEY_LEFT != 0) c.drawRect(d.left, dcy - t / 2, dcx - t / 2, dcy + t / 2, white)
        if (keys and Native.KEY_RIGHT != 0) c.drawRect(dcx + t / 2, dcy - t / 2, d.right, dcy + t / 2, white)
    }

    override fun onDetachedFromWindow() { cancelGear(); super.onDetachedFromWindow() }
}

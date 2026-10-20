package io.gbahandoff

/**
 * Pure (no android.*) model of the virtual pad layout, so it is unit-testable on the JVM.
 *
 * Positions are the control CENTER as a fraction of the view (0..1), so a layout survives different
 * screen sizes. Size is `scale` (0.5..2) times a per-control base size expressed in units
 * U = 10% of the smaller view dimension. Opacity is 0.1..1.
 */
object PadIds {
    const val DPAD = 0; const val A = 1; const val B = 2; const val L = 3
    const val R = 4; const val START = 5; const val SELECT = 6
    const val COUNT = 7
    val NAMES = arrayOf("D-pad", "A", "B", "L", "R", "Start", "Select")
}

class PadLayout(val land: Boolean) {
    val cx = FloatArray(PadIds.COUNT)
    val cy = FloatArray(PadIds.COUNT)
    val scale = FloatArray(PadIds.COUNT)
    val alpha = FloatArray(PadIds.COUNT)

    init { reset() }

    fun reset() {
        val d = if (land) DEF_LAND else DEF_PORT
        for (i in 0 until PadIds.COUNT) {
            cx[i] = d[i * 2]; cy[i] = d[i * 2 + 1]
            scale[i] = 1f; alpha[i] = DEFAULT_ALPHA
        }
    }

    fun unit(w: Float, h: Float) = minOf(w, h) * UNIT_FRAC
    fun halfW(i: Int, w: Float, h: Float) = BASE_W[i] * scale[i] * unit(w, h) / 2f
    fun halfH(i: Int, w: Float, h: Float) = BASE_H[i] * scale[i] * unit(w, h) / 2f
    fun centerXpx(i: Int, w: Float) = cx[i] * w
    fun centerYpx(i: Int, h: Float) = cy[i] * h

    /** Keeps the whole control inside the view. A control bigger than the view is centered. */
    fun clamp(i: Int, w: Float, h: Float) {
        if (w <= 0f || h <= 0f) { cx[i] = cx[i].coerceIn(0f, 1f); cy[i] = cy[i].coerceIn(0f, 1f); return }
        val fx = halfW(i, w, h) / w
        val fy = halfH(i, w, h) / h
        cx[i] = if (fx >= 0.5f) 0.5f else cx[i].coerceIn(fx, 1f - fx)
        cy[i] = if (fy >= 0.5f) 0.5f else cy[i].coerceIn(fy, 1f - fy)
    }

    fun clampAll(w: Float, h: Float) { for (i in 0 until PadIds.COUNT) clamp(i, w, h) }

    /** Moves the control so its center is at pixel (px, py) (clamped). */
    fun moveTo(i: Int, px: Float, py: Float, w: Float, h: Float) {
        if (w <= 0f || h <= 0f) return
        cx[i] = px / w; cy[i] = py / h
        clamp(i, w, h)
    }

    fun setScale(i: Int, s: Float, w: Float, h: Float) {
        scale[i] = s.coerceIn(MIN_SCALE, MAX_SCALE)
        clamp(i, w, h)
    }

    fun setAlpha(i: Int, a: Float) { alpha[i] = a.coerceIn(MIN_ALPHA, MAX_ALPHA) }
    fun setAllAlpha(a: Float) { for (i in 0 until PadIds.COUNT) setAlpha(i, a) }

    /** "i,cx,cy,scale,alpha;..." — plain text, locale independent. */
    fun encode(): String {
        val sb = StringBuilder()
        for (i in 0 until PadIds.COUNT) {
            if (i > 0) sb.append(';')
            sb.append(i).append(',').append(cx[i]).append(',').append(cy[i]).append(',')
                .append(scale[i]).append(',').append(alpha[i])
        }
        return sb.toString()
    }

    /** Tolerant: unknown/garbled records are ignored (control keeps its default); values are clamped. */
    fun decode(s: String?) {
        if (s.isNullOrBlank()) return
        for (rec in s.split(';')) {
            val f = rec.split(',')
            if (f.size != 5) continue
            val i = f[0].toIntOrNull() ?: continue
            if (i !in 0 until PadIds.COUNT) continue
            val v = FloatArray(4)
            var ok = true
            for (k in 0 until 4) {
                val x = f[k + 1].toFloatOrNull()
                if (x == null || x.isNaN() || x.isInfinite()) { ok = false; break }
                v[k] = x
            }
            if (!ok) continue
            cx[i] = v[0].coerceIn(0f, 1f); cy[i] = v[1].coerceIn(0f, 1f)
            scale[i] = v[2].coerceIn(MIN_SCALE, MAX_SCALE)
            alpha[i] = v[3].coerceIn(MIN_ALPHA, MAX_ALPHA)
        }
    }

    companion object {
        const val UNIT_FRAC = 0.1f
        const val MIN_SCALE = 0.5f; const val MAX_SCALE = 2f
        const val MIN_ALPHA = 0.1f; const val MAX_ALPHA = 1f
        const val DEFAULT_ALPHA = 0.5f

        // Base sizes in units U, order: DPAD, A, B, L, R, START, SELECT. ~35% smaller than the first version.
        val BASE_W = floatArrayOf(3.0f, 1.7f, 1.7f, 2.15f, 2.15f, 1.8f, 1.8f)
        val BASE_H = floatArrayOf(3.0f, 1.7f, 1.7f, 1.0f, 1.0f, 1.0f, 1.0f)

        // Default centers (x, y) per control, same order.
        private val DEF_PORT = floatArrayOf(
            0.24f, 0.76f,  0.85f, 0.68f,  0.65f, 0.77f,
            0.17f, 0.38f,  0.83f, 0.38f,  0.60f, 0.93f,  0.40f, 0.93f)
        private val DEF_LAND = floatArrayOf(
            0.13f, 0.66f,  0.93f, 0.56f,  0.84f, 0.72f,
            0.08f, 0.10f,  0.92f, 0.10f,  0.56f, 0.92f,  0.44f, 0.92f)

        fun from(land: Boolean, s: String?) = PadLayout(land).also { it.decode(s) }
    }
}

/** Minimal key-value abstraction (SharedPreferences on Android, a map in tests). */
interface KvStore {
    fun get(key: String): String?
    fun put(key: String, value: String)
}

/** Persists one layout per orientation. */
class PadLayoutStore(private val kv: KvStore) {
    private fun key(land: Boolean) = if (land) "pad_land" else "pad_port"
    fun load(land: Boolean): PadLayout = PadLayout.from(land, kv.get(key(land)))
    fun save(l: PadLayout) = kv.put(key(l.land), l.encode())
}

class PrefsKv(private val p: android.content.SharedPreferences) : KvStore {
    override fun get(key: String): String? = p.getString(key, null)
    override fun put(key: String, value: String) { p.edit().putString(key, value).apply() }
}

package io.gbahandoff

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout

/** Pure (no android.*) rules of the in-game menu, unit-testable. */
object GameMenuLogic {
    enum class Item(val label: String) {
        EDIT("Editar controles"), SETTINGS("Configurações"), SEND("Enviar ao PC"), EXIT("Sair do jogo")
    }

    /**
     * Entries of the sheet. [here] = the game is on the PC; [control] = the phone is only a controller for it
     * (then the pad is visible and can be edited). With the game on the PC and no control mode the pad is
     * hidden, so there is nothing to edit. "Enviar" needs a link and the game on the phone.
     */
    fun items(connected: Boolean, here: Boolean, control: Boolean): List<Item> = buildList {
        if (!here || control) add(Item.EDIT)
        add(Item.SETTINGS)
        if (connected && !here) add(Item.SEND)
        add(Item.EXIT)
    }

    const val BUTTON_DP = 40f
    private const val MARGIN_DP = 6f

    /**
     * Top-left of the menu button: the first free corner of the whole screen (w x h), preferring the top
     * right, avoiding the pad controls' touch areas in [blocked].
     */
    fun buttonPos(w: Float, h: Float, density: Float, blocked: List<FloatArray>): FloatArray {
        val size = BUTTON_DP * density
        return Placement.pick(floatArrayOf(0f, 0f, w, h), size, size, MARGIN_DP * density, blocked, Placement.PREFER_TOP_RIGHT)
    }
}

/** The small semi-transparent "three lines" button; a plain tap opens the menu. */
class MenuButton(ctx: Context, onClick: () -> Unit) : View(ctx) {
    private val d = ctx.resources.displayMetrics.density
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xE6FFFFFF.toInt(); strokeWidth = 2.5f * d; strokeCap = Paint.Cap.ROUND }

    init {
        background = GradientDrawable().apply { setColor(0x66202020); cornerRadius = 10 * d; setStroke(1, 0x44FFFFFF) }
        contentDescription = "Menu"
        isClickable = true
        setOnClickListener { onClick() }
    }

    override fun onMeasure(w: Int, h: Int) {
        val s = (GameMenuLogic.BUTTON_DP * d).toInt()
        setMeasuredDimension(s, s)
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat(); val h = height.toFloat()
        val x0 = w * 0.28f; val x1 = w * 0.72f
        for (i in -1..1) c.drawLine(x0, h / 2 + i * h * 0.17f, x1, h / 2 + i * h * 0.17f, line)
    }
}

/** Dimmed full-screen sheet with the menu entries; a tap outside the entries closes it. */
class MenuSheet(private val ctx: Context, private val onPick: (GameMenuLogic.Item) -> Unit, private val onDismiss: () -> Unit) : FrameLayout(ctx) {
    private val col = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }

    init {
        setBackgroundColor(0x99000000.toInt())
        isClickable = true
        setOnClickListener { hide() }
        addView(col, LayoutParams(LinkUi.dp(ctx, 240f), -2, Gravity.CENTER))
        visibility = GONE
    }

    fun show(items: List<GameMenuLogic.Item>) {
        col.removeAllViews()
        for (it in items) {
            col.addView(LinkUi.chip(ctx, it.label) { hide(false); onPick(it) }.apply {
                textSize = 15f
                minimumHeight = LinkUi.dp(ctx, 48f)
            }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = LinkUi.dp(ctx, 6f) })
        }
        visibility = VISIBLE
    }

    /** [notify]: tell the owner (so it resumes the game); false when an entry was picked and the owner takes over. */
    fun hide(notify: Boolean = true) {
        if (visibility != VISIBLE) return
        visibility = GONE
        if (notify) onDismiss()
    }

    val shown: Boolean get() = visibility == VISIBLE
}

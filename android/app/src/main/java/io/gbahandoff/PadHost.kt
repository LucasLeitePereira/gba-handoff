package io.gbahandoff

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView

/**
 * Hosts the [PadView] plus the layout-editor panel (sliders + buttons, plain Views).
 * [onEditing] is called with true when the editor opens (caller pauses emulation) and false when it
 * closes (caller resumes). While editing, the panel can be moved between top and bottom.
 */
class PadHost(
    context: Context,
    onKeys: (Int) -> Unit,
    private val onEditing: (Boolean) -> Unit,
) : FrameLayout(context) {
    private val d = resources.displayMetrics.density
    private val pad = PadView(context, PadLayoutStore(PrefsKv(context.getSharedPreferences("pad_layout", Context.MODE_PRIVATE))), onKeys)
    private val panel = LinearLayout(context)
    private val title = TextView(context)
    private val sizeBar = SeekBar(context)
    private val sizeVal = TextView(context)
    private val alphaBar = SeekBar(context)
    private val alphaVal = TextView(context)
    private val allBar = SeekBar(context)
    private val allVal = TextView(context)
    private var syncing = false
    private var panelTop = true

    val editing get() = pad.editing

    /** Rects (this view's coordinates) where a touch would hit a control; floating widgets stay out of them. */
    fun occupiedRects(out: MutableList<FloatArray>) = pad.occupiedRects(out)

    init {
        addView(pad, LayoutParams(-1, -1))
        panel.orientation = LinearLayout.VERTICAL
        panel.setBackgroundColor(0xE6181818.toInt())
        val p = (8 * d).toInt()
        panel.setPadding(p, p / 2, p, p / 2)
        panel.isClickable = true        // swallow touches so they don't reach the pad under the panel
        panel.visibility = View.GONE

        title.setTextColor(Color.WHITE); title.textSize = 14f
        panel.addView(title)
        sizeBar.max = 150; alphaBar.max = 90; allBar.max = 90
        panel.addView(row("Tamanho", sizeBar, sizeVal))
        panel.addView(row("Opacidade", alphaBar, alphaVal))
        panel.addView(row("Todos (opac.)", allBar, allVal))

        val btns = LinearLayout(context)
        btns.addView(button("Restaurar padrão") { pad.resetLayout(); sync() }, LinearLayout.LayoutParams(0, -2, 1f))
        btns.addView(button("Painel ↕") {
            panelTop = !panelTop
            (panel.layoutParams as LayoutParams).gravity = (if (panelTop) Gravity.TOP else Gravity.BOTTOM) or Gravity.CENTER_HORIZONTAL
            panel.requestLayout()
        }, LinearLayout.LayoutParams(0, -2, 1f))
        btns.addView(button("Concluir") { exitEdit() }, LinearLayout.LayoutParams(0, -2, 1f))
        panel.addView(btns)
        addView(panel, LayoutParams(-1, -2, Gravity.TOP or Gravity.CENTER_HORIZONTAL))

        sizeBar.setOnSeekBarChangeListener(listener({ pad.setSelectedScale(0.5f + it / 100f) }) { pad.save() })
        alphaBar.setOnSeekBarChangeListener(listener({ pad.setSelectedAlpha(0.1f + it / 100f) }) { pad.save() })
        allBar.setOnSeekBarChangeListener(listener({ pad.setAllAlpha(0.1f + it / 100f); syncing = true; alphaBar.progress = it; syncing = false }) { pad.save() })

        pad.onSelectionChanged = { sync() }
    }

    private fun row(name: String, bar: SeekBar, value: TextView): LinearLayout {
        val r = LinearLayout(context)
        r.gravity = Gravity.CENTER_VERTICAL
        val t = TextView(context); t.text = name; t.setTextColor(Color.WHITE); t.textSize = 12f
        r.addView(t, LinearLayout.LayoutParams((84 * d).toInt(), -2))
        r.addView(bar, LinearLayout.LayoutParams(0, (32 * d).toInt(), 1f))
        value.setTextColor(Color.WHITE); value.textSize = 12f; value.gravity = Gravity.END
        r.addView(value, LinearLayout.LayoutParams((40 * d).toInt(), -2))
        return r
    }

    private fun button(label: String, click: () -> Unit) = Button(context).apply {
        text = label; textSize = 12f; isAllCaps = false
        minHeight = 0; minimumHeight = (36 * d).toInt()
        setOnClickListener { click() }
    }

    private fun listener(onChange: (Int) -> Unit, onStop: () -> Unit) = object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) { if (fromUser && !syncing) { onChange(p); sync(false) } }
        override fun onStartTrackingTouch(s: SeekBar) {}
        override fun onStopTrackingTouch(s: SeekBar) { onStop() }
    }

    /** Refreshes the sliders/labels from the model. [full] also moves the bars. */
    private fun sync(full: Boolean = true) {
        syncing = true
        val sel = pad.selected
        title.text = if (sel >= 0) "Editando: ${PadIds.NAMES[sel]}  (arraste para mover)" else "Toque num controle para selecioná-lo"
        sizeBar.isEnabled = sel >= 0; alphaBar.isEnabled = sel >= 0
        if (full) {
            sizeBar.progress = ((pad.selectedScale() - 0.5f) * 100).toInt()
            alphaBar.progress = ((pad.selectedAlpha() - 0.1f) * 100).toInt()
            allBar.progress = ((pad.selectedAlpha() - 0.1f) * 100).toInt()
        }
        sizeVal.text = "${(pad.selectedScale() * 100).toInt()}%"
        alphaVal.text = "${(pad.selectedAlpha() * 100).toInt()}%"
        allVal.text = "${(allBar.progress + 10)}%"
        syncing = false
    }

    /** Opens the layout editor (from the game menu). */
    fun enterEdit() {
        if (pad.editing) return
        pad.editing = true
        panel.visibility = View.VISIBLE
        sync()
        onEditing(true)
    }

    /** Closes the editor if open. Returns true if it was open (use for Back handling). */
    fun exitEdit(): Boolean {
        if (!pad.editing) return false
        pad.save()
        pad.editing = false
        panel.visibility = View.GONE
        onEditing(false)
        return true
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        val lp = panel.layoutParams as LayoutParams
        lp.width = minOf(w, (420 * d).toInt())
        panel.layoutParams = lp
    }
}

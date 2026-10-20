package io.gbahandoff

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView

/**
 * Settings of the link (compact, over the game list). "Sempre manter a versão mais recente": a version
 * conflict is resolved by itself in favour of the newest save (the other one still becomes a backup).
 * The PC has the same option; when they differ the app asks which one to use for both.
 */
class SettingsPanel(private val ctx: Context, private val onClose: () -> Unit) : FrameLayout(ctx) {
    private val sw = Switch(ctx).apply { text = "Sempre manter a versão mais recente"; textSize = 13f; setTextColor(Color.WHITE) }
    private var quiet = false

    init {
        setBackgroundColor(0xF2000000.toInt())
        isClickable = true
        val col = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        col.addView(TextView(ctx).apply { text = "Configurações"; textSize = 16f; setTextColor(Color.WHITE) })
        col.addView(sw, LinearLayout.LayoutParams(-1, -2).apply { topMargin = LinkUi.dp(ctx, 14f) })
        col.addView(TextView(ctx).apply {
            text = "Desligado: em caso de conflito o app pergunta qual versão manter. Ligado: mantém a mais recente " +
                "(pela hora do salvamento) e guarda a outra como backup. Se o PC estiver com outra escolha, o app pergunta qual usar nos dois."
            textSize = 11f; setTextColor(0xB3FFFFFF.toInt())
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = LinkUi.dp(ctx, 6f) })
        col.addView(LinkUi.chip(ctx, "Fechar") { hide(); onClose() }, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 16f) })
        addView(col, LayoutParams(-1, -2, Gravity.CENTER).apply { leftMargin = LinkUi.dp(ctx, 20f); rightMargin = LinkUi.dp(ctx, 20f) })
        sw.setOnCheckedChangeListener { _, on -> if (!quiet) LinkClient.setPreferNewest(ctx, on) }
        visibility = GONE
    }

    fun show() {
        quiet = true; sw.isChecked = LinkClient.preferNewest(ctx); quiet = false
        visibility = VISIBLE
    }
    fun hide() { visibility = GONE }
    /** The choice made in the "settings differ" question changes the value behind the panel's back. */
    fun refresh() { if (visibility == VISIBLE) { quiet = true; sw.isChecked = LinkClient.preferNewest(ctx); quiet = false } }
    val shown: Boolean get() = visibility == VISIBLE
}

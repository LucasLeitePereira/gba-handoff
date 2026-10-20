package io.gbahandoff

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

/** Small helpers for the compact, semi-transparent look of the link UI. */
object LinkUi {
    fun dp(ctx: Context, v: Float) = (v * ctx.resources.displayMetrics.density + 0.5f).toInt()

    fun chip(ctx: Context, text: String, onClick: () -> Unit): Button = Button(ctx).apply {
        this.text = text
        textSize = 12f
        isAllCaps = false
        minHeight = 0; minimumHeight = 0; minWidth = 0; minimumWidth = 0
        setPadding(dp(ctx, 10f), dp(ctx, 4f), dp(ctx, 10f), dp(ctx, 4f))
        setTextColor(Color.WHITE)
        background = GradientDrawable().apply { setColor(0x80202020.toInt()); cornerRadius = dp(ctx, 12f).toFloat(); setStroke(1, 0x55FFFFFF) }
        setOnClickListener { onClick() }
    }

    /**
     * Drives a [ChoicePanel] from the engine state: the "continue on" question (per game) wins over the
     * "settings differ" one. Answers: key "p:<sha>" (first = phone) or "s" (first = keep the newest).
     */
    fun updateChoice(panel: ChoicePanel, st: LinkState, titleOf: (String) -> String) {
        val place = st.places.entries.firstOrNull()
        val d = st.settingsDiff
        when {
            place != null -> panel.show("p:" + place.key, "Continuar no:", titleOf(place.key),
                "Celular", "PC", enabled = !place.value)
            d != null -> panel.show("s", "As configura\u00e7\u00f5es dos dois aparelhos s\u00e3o diferentes",
                "Sempre manter a vers\u00e3o mais recente: celular ${if (d.first) "ligado" else "desligado"}, PC ${if (d.second) "ligado" else "desligado"}",
                "Usar \u201cmais recente\u201d", "Usar \u201cperguntar\u201d")
            else -> panel.hide()
        }
    }

    fun answer(ctx: Context, key: String, first: Boolean) {
        if (key.startsWith("p:")) LinkClient.place(key.substring(2), !first)
        else if (key == "s") LinkClient.chooseSettings(ctx, first)
    }

    fun label(ctx: Context, size: Float = 12f, color: Int = Color.WHITE) = TextView(ctx).apply {
        textSize = size; setTextColor(color); gravity = Gravity.CENTER
    }
}

/** What the "game is on the PC" screens say (pure; shared by the game and list screens). */
object OnPcText {
    /** Line under the title: synchronised (cable) or not (cable out), with the time of the last copy. */
    fun sync(connected: Boolean, ts: Long): String {
        val t = if (ts > 0) Fmt.dateTime(ts) else "?"
        return if (connected) "sincronizado $t" else "n\u00e3o sincronizado (\u00faltima c\u00f3pia $t)"
    }
    /** Shown instead of "Jogar no celular" when the ROM of the game that is on the PC is not on this phone. */
    const val NO_ROM = "Este jogo n\u00e3o est\u00e1 no celular"
    fun refusal(status: Int, pcProto: Int): String = when (status) {
        1 -> "O PC usa outra vers\u00e3o do protocolo (v$pcProto): atualize o app do PC e o do celular."
        2 -> "O emulador (mGBA) do PC \u00e9 diferente do deste celular: atualize o app do PC e o do celular."
        else -> "O PC recusou a conex\u00e3o."
    }
}

/**
 * Full-screen "No PC" (black: the phone is not emulating and shows no game picture): the sync state, "Jogar no
 * celular" (only when the ROM is here), the optional "Usar como controle" switch and, with the cable out, "Jogar aqui mesmo assim".
 */
class OnPcScreen(
    private val ctx: Context,
    onReturn: () -> Unit, onPlayHere: () -> Unit, onControl: (Boolean) -> Unit,
) : FrameLayout(ctx) {
    private val title = LinkUi.label(ctx, 15f)
    private val sub = LinkUi.label(ctx, 11f, 0xCCFFFFFF.toInt())
    private val back = LinkUi.chip(ctx, "Jogar no celular", onReturn)
    private val here = LinkUi.chip(ctx, "Jogar aqui mesmo assim", onPlayHere)
    private val ctl = android.widget.Switch(ctx).apply {
        text = "Usar como controle"; textSize = 12f; setTextColor(Color.WHITE)
    }
    private var quiet = false

    init {
        setBackgroundColor(0xFF000000.toInt())
        isClickable = true
        val col = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER }
        col.addView(title)
        col.addView(sub, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 2f) })
        col.addView(ctl, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 10f) })
        col.addView(back, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 10f) })
        col.addView(here, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 6f) })
        addView(col, LayoutParams(-2, -2, Gravity.CENTER))
        ctl.setOnCheckedChangeListener { _, on -> if (!quiet) onControl(on) }
        visibility = GONE
    }

    fun show(connected: Boolean, ts: Long, waitingReturn: Boolean, control: Boolean, romHere: Boolean = true) {
        visibility = VISIBLE
        getChildAt(0).visibility = VISIBLE
        title.text = if (connected) "No PC" else "O jogo continua no PC"
        sub.text = if (waitingReturn) "Trazendo de volta..." else OnPcText.sync(connected, ts) + if (romHere) "" else "\n" + OnPcText.NO_ROM
        back.visibility = if (connected && romHere) VISIBLE else GONE
        back.isEnabled = !waitingReturn; back.alpha = if (waitingReturn) 0.5f else 1f
        here.visibility = if (connected || !romHere) GONE else VISIBLE
        ctl.visibility = if (connected) VISIBLE else GONE
        quiet = true; ctl.isChecked = control; quiet = false
    }
    fun hide() { visibility = GONE }
    /** Control mode: keep the black, drop the card (the pad and the pill are on top). */
    fun hideCard() { getChildAt(0).visibility = GONE }
}

/**
 * Control mode: the black screen shows only the pad; this compact, semi-transparent pill (state + "Sair do
 * controle") sits in a free corner. The container is NOT clickable so touches elsewhere reach the pad.
 */
class OnPcPill(ctx: Context, onExit: () -> Unit) : LinearLayout(ctx) {
    private val sub = LinkUi.label(ctx, 11f, 0xE6FFFFFF.toInt())
    private val btn = LinkUi.chip(ctx, "Sair do controle", onExit)

    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        isClickable = false
        background = GradientDrawable().apply { setColor(0x66000000); cornerRadius = LinkUi.dp(ctx, 12f).toFloat() }
        setPadding(LinkUi.dp(ctx, 8f), LinkUi.dp(ctx, 2f), LinkUi.dp(ctx, 2f), LinkUi.dp(ctx, 2f))
        addView(sub, LayoutParams(-2, -2).apply { rightMargin = LinkUi.dp(ctx, 6f) })
        addView(btn, LayoutParams(-2, -2))
        visibility = GONE
    }

    fun show(connected: Boolean, ts: Long) {
        visibility = VISIBLE
        sub.text = "No PC \u2022 " + OnPcText.sync(connected, ts).substringBefore(" (")
    }
    fun hide() { visibility = GONE }
}

/** A two-way question over everything: "Continuar no: Celular | PC", or which default setting to use. */
class ChoicePanel(private val ctx: Context, private val onChoice: (key: String, first: Boolean) -> Unit) : FrameLayout(ctx) {
    private val title = LinkUi.label(ctx, 14f)
    private val sub = LinkUi.label(ctx, 11f, 0xCCFFFFFF.toInt())
    private val a = LinkUi.chip(ctx, "") { key?.let { onChoice(it, true) } }
    private val b = LinkUi.chip(ctx, "") { key?.let { onChoice(it, false) } }
    private var key: String? = null

    init {
        setBackgroundColor(0xF0000000.toInt())
        isClickable = true
        val col = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER }
        col.addView(title)
        col.addView(sub, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 4f) })
        val row = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        row.addView(a, LinearLayout.LayoutParams(-2, -2).apply { rightMargin = LinkUi.dp(ctx, 8f) })
        row.addView(b, LinearLayout.LayoutParams(-2, -2))
        col.addView(row, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 12f) })
        addView(col, LayoutParams(-2, -2, Gravity.CENTER).apply { leftMargin = LinkUi.dp(ctx, 12f); rightMargin = LinkUi.dp(ctx, 12f) })
        visibility = GONE
    }

    fun show(k: String, t: String, s: String, first: String, second: String, enabled: Boolean = true) {
        key = k; visibility = VISIBLE
        title.text = t; sub.text = s; a.text = first; b.text = second
        a.isEnabled = enabled; b.isEnabled = enabled
        a.alpha = if (enabled) 1f else 0.5f; b.alpha = a.alpha
    }
    fun hide() { visibility = GONE; key = null }
    val shown: Boolean get() = visibility == VISIBLE
}

/** Both versions of a game side by side; the user keeps one. The loser becomes a backup (3 kept). */
class ConflictPanel(private val ctx: Context, private val onChoice: (sha1: String, keepPhone: Boolean) -> Unit) : FrameLayout(ctx) {
    private val title = LinkUi.label(ctx, 13f)
    private val phoneInfo = LinkUi.label(ctx, 11f)
    private val pcInfo = LinkUi.label(ctx, 11f)
    private val phoneImg = ImageView(ctx)
    private val pcImg = ImageView(ctx)
    private val keepPhone = LinkUi.chip(ctx, "Manter do celular") { cur?.let { onChoice(it, true) } }
    private val keepPc = LinkUi.chip(ctx, "Manter do PC") { cur?.let { onChoice(it, false) } }
    private var cur: String? = null
    private var keyPhone = ""
    private var keyPc = ""

    init {
        setBackgroundColor(0xF0000000.toInt())
        isClickable = true
        val col = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER }
        col.addView(title)
        val row = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        row.addView(side(phoneImg, phoneInfo, keepPhone), LinearLayout.LayoutParams(0, -2, 1f))
        row.addView(side(pcImg, pcInfo, keepPc), LinearLayout.LayoutParams(0, -2, 1f))
        col.addView(row, LinearLayout.LayoutParams(-1, -2).apply { topMargin = LinkUi.dp(ctx, 8f) })
        col.addView(LinkUi.label(ctx, 10f, 0x99FFFFFF.toInt()).apply { text = "A versão descartada fica como backup" },
            LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 8f) })
        addView(col, LayoutParams(-1, -2, Gravity.CENTER).apply { leftMargin = LinkUi.dp(ctx, 12f); rightMargin = LinkUi.dp(ctx, 12f) })
        visibility = GONE
    }

    private fun side(img: ImageView, info: TextView, btn: Button) = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        setPadding(LinkUi.dp(ctx, 4f), 0, LinkUi.dp(ctx, 4f), 0)
        img.scaleType = ImageView.ScaleType.FIT_CENTER
        img.adjustViewBounds = true
        img.setBackgroundColor(0xFF111111.toInt())
        addView(img, LinearLayout.LayoutParams(-1, LinkUi.dp(ctx, 96f)))
        addView(info, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 4f) })
        addView(btn, LinearLayout.LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 6f) })
    }

    private fun describe(kind: Int, ts: Long, frame: Long) =
        "${Fmt.device(kind)}\n${Fmt.dateTime(ts)}\n${Fmt.playTime(frame)} de jogo"

    fun show(c: Conflict, gameTitle: String) {
        cur = c.sha1
        visibility = VISIBLE
        title.text = "Conflito: $gameTitle"
        phoneInfo.text = describe(c.localDev, c.localTs, c.localFrame)
        pcInfo.text = if (c.haveRemote) describe(c.remoteDev, c.remoteTs, c.remoteFrame)
                      else if (c.waitingPc) "PC\naguardando a resposta..." else "PC\nsem dados"
        keepPhone.isEnabled = true
        keepPc.isEnabled = c.haveRemote || c.waitingPc
        keepPc.alpha = if (keepPc.isEnabled) 1f else 0.5f
        // pictures are fetched once per version (the poll runs every few hundred ms)
        val kp = "${c.sha1}/${c.localTs}"
        if (kp != keyPhone) { keyPhone = kp; phoneImg.setImageBitmap(LinkClient.conflictThumb(c.sha1, false)) }
        val kr = "${c.sha1}/${c.remoteTs}/${c.remoteThumb}"
        if (kr != keyPc) { keyPc = kr; pcImg.setImageBitmap(if (c.remoteThumb) LinkClient.conflictThumb(c.sha1, true) else null) }
    }

    fun hide() { visibility = GONE; cur = null; keyPhone = ""; keyPc = "" }
    val shown: Boolean get() = visibility == VISIBLE
}

/** One-line notice that fades by itself. */
class NoticeLine(ctx: Context) : TextView(ctx) {
    private val hideTask = Runnable { visibility = GONE }
    init {
        textSize = 11f; setTextColor(Color.WHITE); gravity = Gravity.CENTER
        setPadding(LinkUi.dp(ctx, 10f), LinkUi.dp(ctx, 3f), LinkUi.dp(ctx, 10f), LinkUi.dp(ctx, 3f))
        background = GradientDrawable().apply { setColor(0x99000000.toInt()); cornerRadius = LinkUi.dp(ctx, 10f).toFloat() }
        visibility = GONE
    }
    fun say(msg: String) { text = msg; visibility = VISIBLE; removeCallbacks(hideTask); postDelayed(hideTask, 5000) }
    override fun onDetachedFromWindow() { removeCallbacks(hideTask); super.onDetachedFromWindow() }
}


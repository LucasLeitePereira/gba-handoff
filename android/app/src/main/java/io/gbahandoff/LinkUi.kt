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

    fun label(ctx: Context, size: Float = 12f, color: Int = Color.WHITE) = TextView(ctx).apply {
        textSize = size; setTextColor(color); gravity = Gravity.CENTER
    }
}

/** "Jogando no PC": covers the game picture while the PC holds the game. */
class OnPcOverlay(ctx: Context, onReturn: () -> Unit) : LinearLayout(ctx) {
    private val sub = LinkUi.label(ctx, 11f, 0xCCFFFFFF.toInt())
    private val btn = LinkUi.chip(ctx, "Voltar para o celular", onReturn)

    init {
        orientation = VERTICAL
        gravity = Gravity.CENTER
        setBackgroundColor(0xE0000000.toInt())
        isClickable = true          // swallow touches: the pad under it is not in use
        addView(LinkUi.label(ctx, 15f).apply { text = "Jogando no PC" })
        addView(sub)
        addView(btn, LayoutParams(-2, -2).apply { topMargin = LinkUi.dp(ctx, 10f) })
        visibility = GONE
    }

    fun show(waitingReturn: Boolean) {
        visibility = VISIBLE
        sub.text = if (waitingReturn) "Trazendo de volta..." else "O progresso volta para o celular ao retornar"
        btn.isEnabled = !waitingReturn
        btn.alpha = if (waitingReturn) 0.5f else 1f
    }
    fun hide() { visibility = GONE }
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


package io.gbahandoff

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * Game list (ROMs picked here or received from the PC, plus anything with saved progress), link
 * status, conflict resolution and the ROM picker (SAF). When the PC hands a game back that is not
 * open, it is opened here.
 */
class MainActivity : Activity() {
    private lateinit var linkText: TextView
    private lateinit var list: LinearLayout
    private lateinit var notice: NoticeLine
    private lateinit var conflictPanel: ConflictPanel
    private lateinit var roms: RomLibrary
    private val ui = Handler(Looper.getMainLooper())
    private var lastRows = ""
    private val refresh = object : Runnable {
        override fun run() { update(); ui.postDelayed(this, 500) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        roms = LinkClient.boot(this)
        val dp = { v: Float -> LinkUi.dp(this, v) }
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14f), dp(12f), dp(14f), dp(12f))
        }
        col.addView(TextView(this).apply { text = getString(R.string.app_name); textSize = 20f })
        linkText = TextView(this).apply { textSize = 12f; setPadding(0, dp(2f), 0, dp(6f)) }
        col.addView(linkText)
        col.addView(Button(this).apply {
            text = "Adicionar ROM (.gba)"
            isAllCaps = false
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
                }, REQ_ROM)
            }
        })
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        col.addView(ScrollView(this).apply { addView(list) }, LinearLayout.LayoutParams(-1, 0, 1f))

        val root = FrameLayout(this)
        root.addView(col, FrameLayout.LayoutParams(-1, -1))
        notice = NoticeLine(this)
        root.addView(notice, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply { bottomMargin = dp(12f) })
        conflictPanel = ConflictPanel(this) { h, keepPhone -> LinkClient.resolve(h, keepPhone) }
        root.addView(conflictPanel, FrameLayout.LayoutParams(-1, -1))
        setContentView(root)
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(refresh)
        LinkClient.onBackground()
    }

    override fun onResume() {
        super.onResume()
        LinkClient.onForeground(this)
        lastRows = ""
        ui.post(refresh)
    }

    private fun update() {
        val st = LinkClient.state()
        linkText.text = when (st.status) {
            LinkClient.CONNECTED -> "Conectado ao PC" + (st.peer.takeIf { it.isNotBlank() }?.let { ": $it" } ?: "")
            LinkClient.REFUSED -> "PC recusou: versão do emulador incompatível"
            else -> "Procurando o PC (cabo USB + adb)..."
        }
        st.notices.lastOrNull()?.let { notice.say(it) }
        // the PC handed a game back and none is open: open it
        st.openRequest?.let { sha ->
            if (roms.find(sha) != null) {
                LinkClient.ackOpen(sha)
                startActivity(Intent(this, GameActivity::class.java).putExtra(GameActivity.EXTRA_SHA1, sha))
                return
            }
        }
        val c = st.conflicts.firstOrNull()
        if (c != null) conflictPanel.show(c, roms.find(c.sha1)?.title?.ifBlank { null } ?: "jogo ${Fmt.shortSha(c.sha1)}") else conflictPanel.hide()
        val games = GameList.build(roms.all(), filesDir.resolve("games"), st)
        val key = games.joinToString("|") { "${it.sha1}:${it.title}:${it.progressMs}:${it.onPc}:${it.conflict}:${it.receivingPct}:${it.hasRom}" }
        if (key != lastRows) { lastRows = key; render(games) }
    }

    private fun render(games: List<GameEntry>) {
        list.removeAllViews()
        if (games.isEmpty()) {
            list.addView(TextView(this).apply { text = "Nenhum jogo ainda. Adicione uma ROM ou conecte ao PC."; textSize = 12f; setPadding(0, LinkUi.dp(this@MainActivity, 12f), 0, 0) })
            return
        }
        for (g in games) list.addView(row(g))
    }

    private fun row(g: GameEntry): View {
        val dp = { v: Float -> LinkUi.dp(this, v) }
        val state = when {
            g.conflict -> "conflito de versões"
            g.onPc -> "jogando no PC (toque para voltar)"
            g.receivingPct != null -> "recebendo a ROM do PC ${g.receivingPct}%"
            !g.hasRom -> "sem a ROM neste celular"
            g.progressMs > 0 -> "salvo " + Fmt.dateTime(g.progressMs)
            else -> "novo"
        }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(4f), dp(8f), dp(4f), dp(8f))
            addView(TextView(context).apply { text = g.title; textSize = 15f })
            addView(TextView(context).apply {
                text = state; textSize = 11f
                setTextColor(if (g.conflict) Color.rgb(0xC6, 0x28, 0x28) else 0xFF777777.toInt())
            })
            val usable = g.hasRom && !g.onPc && !g.conflict
            isEnabled = usable
            alpha = if (usable || g.onPc) 1f else 0.6f
            setOnClickListener { if (usable) startActivity(Intent(this@MainActivity, GameActivity::class.java).putExtra(GameActivity.EXTRA_SHA1, g.sha1)) }
            if (g.onPc) setOnClickListener { LinkClient.doReturn() }
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode == REQ_ROM && resultCode == RESULT_OK && uri != null) {
            try {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (_: SecurityException) { /* provider without persistable grants: still works this session */ }
            // GameActivity registers it (sha1, size, title) once the core has hashed it
            startActivity(Intent(this, GameActivity::class.java).setData(uri))
        }
    }

    private companion object { const val REQ_ROM = 1 }
}


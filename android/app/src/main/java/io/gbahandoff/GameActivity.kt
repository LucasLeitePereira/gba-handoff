package io.gbahandoff

import android.app.Activity
import android.os.Bundle
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.net.Uri
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.FrameLayout
import android.widget.Toast
import java.io.File

/**
 * Game screen. Lifecycle: load ROM (+ resume state/sav) in onCreate; emulation runs only between
 * onResume and onPause; onPause also writes state + .sav to internal storage (auto-save on exit).
 */
class GameActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surface: SurfaceView
    private lateinit var root: FrameLayout
    private var sha1: String? = null
    private var padHost: PadHost? = null
    private var curW = 0
    private var curH = 0
    private lateinit var roms: RomLibrary
    private lateinit var onPcScreen: OnPcScreen
    private lateinit var onPcPill: OnPcPill
    private lateinit var choicePanel: ChoicePanel
    private lateinit var conflictPanel: ConflictPanel
    private lateinit var notice: NoticeLine
    private lateinit var sendBtn: View
    private lateinit var menuBtn: MenuButton
    private lateinit var menuSheet: MenuSheet
    private lateinit var settingsPanel: SettingsPanel
    private var menuPaused = false   // the menu sheet / settings are open: the game is paused until they close
    private val ui = Handler(Looper.getMainLooper())
    private val poll = object : Runnable {
        override fun run() { refreshLink(); ui.postDelayed(this, POLL_MS) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        root = FrameLayout(this).apply { setBackgroundColor(0xFF000000.toInt()) }
        surface = SurfaceView(this)
        surface.holder.addCallback(this)
        root.addView(surface, FrameLayout.LayoutParams(GBA_W, GBA_H, Gravity.TOP or Gravity.CENTER_HORIZONTAL))
        // black cover above the surface (which keeps the last frame): the game picture is never shown while the game is on the PC
        onPcScreen = OnPcScreen(this, { LinkClient.doReturn() }, { confirmPlayHere() }) { on -> LinkClient.setControlMode(this, on); refreshLink() }
        root.addView(onPcScreen, FrameLayout.LayoutParams(-1, -1))
        padHost = PadHost(this, { Native.setKeys(it); LinkClient.sendKeys(it) }) { editing ->   // pad layout editor: pause while editing
            if (editing) Native.stop() else if (sha1 != null) Native.start()
        }
        root.addView(padHost, FrameLayout.LayoutParams(-1, -1))
        roms = LinkClient.boot(this)
        menuBtn = MenuButton(this) { openMenu() }
        root.addView(menuBtn, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        // link UI above the pad: compact and semi-transparent
        val dp = { v: Float -> LinkUi.dp(this, v) }
        sendBtn = LinkUi.chip(this, "Enviar ao PC") { LinkClient.sendToPc() }.apply { visibility = View.GONE }
        root.addView(sendBtn, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        notice = NoticeLine(this)
        root.addView(notice, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply { topMargin = dp(6f) })
        onPcPill = OnPcPill(this) { LinkClient.setControlMode(this, false); refreshLink() }
        root.addView(onPcPill, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START))
        choicePanel = ChoicePanel(this) { k, first -> LinkUi.answer(this, k, first); settingsPanel.refresh() }
        root.addView(choicePanel, FrameLayout.LayoutParams(-1, -1))
        conflictPanel = ConflictPanel(this) { h, keepPhone -> LinkClient.resolve(h, keepPhone) }
        root.addView(conflictPanel, FrameLayout.LayoutParams(-1, -1))
        menuSheet = MenuSheet(this, { onMenuPick(it) }) { resumeFromMenu() }
        root.addView(menuSheet, FrameLayout.LayoutParams(-1, -1))
        settingsPanel = SettingsPanel(this) { resumeFromMenu() }
        root.addView(settingsPanel, FrameLayout.LayoutParams(-1, -1))
        root.addOnLayoutChangeListener { _, l, t, r, b, _, _, _, _ -> layoutScreen(r - l, b - t) }
        setContentView(root)
        hideSystemBars()

        if (!Native.create()) return fail("Falha ao criar o core")
        // a ROM picked now (uri) or one the library knows (sha1: received from the PC, or handed back)
        val wanted = intent.getStringExtra(EXTRA_SHA1)
        val source = intent.data?.toString() ?: wanted?.let { roms.find(it)?.source } ?: return fail("Nenhuma ROM")
        val rc = try {
            openRom(source)!!.use { Native.loadRomFd(it.fd) }
        } catch (e: Exception) { -1000 }
        if (rc != 0) return fail("ROM inválida ($rc)")
        val h = Native.romSha1()!!
        sha1 = h
        if (wanted != null && wanted != h) return fail("ROM não confere com o jogo pedido")
        if (roms.find(h) == null || intent.data != null) registerPicked(h, source)
        // Continue where the player left off: the sync engine's store (state + sav, else the cold .sav)
        if (Native.resumeStored() < 0) Toast.makeText(this, "Não foi possível retomar o progresso salvo", Toast.LENGTH_LONG).show()
    }

    private fun openRom(source: String): ParcelFileDescriptor? =
        if (source.startsWith("content://")) contentResolver.openFileDescriptor(Uri.parse(source), "r")
        else ParcelFileDescriptor.open(File(source.removePrefix("file://")), ParcelFileDescriptor.MODE_READ_ONLY)

    /** Remember where this ROM can be read again (the engine needs it to send the ROM to the PC). */
    private fun registerPicked(h: String, source: String) {
        Thread {
            try {
                val size = openRom(source)?.use { it.statSize } ?: return@Thread
                val head = LinkClient.openSource(this, source)?.use { ins -> ByteArray(0xAC).also { var n = 0; while (n < it.size) { val r = ins.read(it, n, it.size - n); if (r < 0) break; n += r } } }
                roms.register(RomLibrary.Rom(h, size, source, head?.let(RomLibrary::headerTitle) ?: ""))
                LinkClient.publishRoms()
            } catch (_: Exception) { }
        }.also { it.isDaemon = true }.start()
    }

    /** Tap on the menu button: pause the game and show the sheet. */
    private fun openMenu() {
        if (padHost?.editing == true || menuPaused) return
        val h = sha1 ?: return
        val st = LinkClient.state()
        val here = h in st.onPc
        menuPaused = true
        Native.stop()
        Native.setKeys(0); LinkClient.sendKeys(0)
        menuSheet.show(GameMenuLogic.items(st.status == LinkClient.CONNECTED, here, LinkClient.controlMode(this)))
    }

    private fun onMenuPick(item: GameMenuLogic.Item) {
        when (item) {
            GameMenuLogic.Item.EDIT -> { menuPaused = false; padHost?.enterEdit() }   // the editor keeps the game paused
            GameMenuLogic.Item.SETTINGS -> settingsPanel.show()                      // still paused until it closes
            GameMenuLogic.Item.SEND -> { LinkClient.sendToPc(); resumeFromMenu() }
            GameMenuLogic.Item.EXIT -> { menuPaused = false; finish() }              // onPause saves
        }
    }

    private fun resumeFromMenu() {
        if (!menuPaused) return
        menuPaused = false
        if (sha1 != null && padHost?.editing != true) Native.start()
    }

    private fun refreshLink() {
        val h = sha1 ?: return
        val st = LinkClient.state()
        st.notices.lastOrNull()?.let { notice.say(it) }
        val here = h in st.onPc
        val connected = st.status == LinkClient.CONNECTED
        val control = LinkClient.controlMode(this)
        val ts = st.onPcTs[h] ?: 0L
        if (here) {
            onPcScreen.show(connected, ts, h in st.waiting, control)
            if (control) onPcPill.show(connected, ts) else onPcPill.hide()
        } else { onPcScreen.hide(); onPcPill.hide() }
        padHost?.visibility = if (here && !control) View.GONE else View.VISIBLE   // the pad only drives the PC in control mode
        onPcScreen.visibility = if (here) View.VISIBLE else View.GONE
        // the cover is part of the screen: in control mode only the pad (and the pill) stay on the black
        if (here && control) onPcScreen.hideCard()
        sendBtn.visibility = if (connected && !here && h !in st.waiting && !conflictPanel.shown && !choicePanel.shown) View.VISIBLE else View.GONE
        placeFloating()
        val c = st.conflicts.firstOrNull()
        if (c != null) conflictPanel.show(c, roms.find(c.sha1)?.title?.ifBlank { null } ?: "jogo ${Fmt.shortSha(c.sha1)}") else conflictPanel.hide()
        if (c != null) choicePanel.hide()
        else LinkUi.updateChoice(choicePanel, st) { sha -> roms.find(sha)?.title?.ifBlank { null } ?: "jogo ${Fmt.shortSha(sha)}" }
        settingsPanel.refresh()
    }

    /** Cable out and the game is on the PC: playing here makes a second version of it. Say so first. */
    private fun confirmPlayHere() {
        android.app.AlertDialog.Builder(this)
            .setTitle("Jogar aqui mesmo assim?")
            .setMessage("O PC continua com a vers\u00e3o dele. Se os dois avan\u00e7arem, na pr\u00f3xima conex\u00e3o voc\u00ea escolhe qual manter (a outra vira backup).\n\nO jogo ser\u00e1 carregado da \u00faltima c\u00f3pia sincronizada.")
            .setPositiveButton("Jogar aqui") { _, _ -> if (LinkClient.playHere() == 0) refreshLink() }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    private fun fail(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
        finish()
    }

    /** Integer scale: biggest k with 240k x 160k inside the top area (half of the screen in portrait). */
    private fun layoutScreen(w: Int, h: Int) {
        if (w == curW && h == curH || w == 0) return
        curW = w; curH = h
        val availH = if (w > h) h else h / 2
        val k = maxOf(1, minOf(w / GBA_W, availH / GBA_H))
        val lp = surface.layoutParams as FrameLayout.LayoutParams
        lp.width = GBA_W * k; lp.height = GBA_H * k
        lp.gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
        lp.topMargin = if (w > h) (h - lp.height) / 2 else 0
        surface.layoutParams = lp
        surface.holder.setFixedSize(lp.width, lp.height) // 1 buffer px == 1 screen px: no compositor scaling
        placeFloating()
    }

    /**
     * The "playing on the PC" pill and the "send to PC" chip are clickable, so they must not sit on a
     * pad control: put each one in a free corner of the game picture (re-evaluated on every poll, so
     * moving a control in the editor is picked up).
     */
    private fun placeFloating() {
        val sv = surface.layoutParams as FrameLayout.LayoutParams
        if (curW == 0 || sv.width == 0) return
        val left = ((curW - sv.width) / 2).toFloat(); val top = sv.topMargin.toFloat()
        val area = floatArrayOf(left, top, left + sv.width, top + sv.height)
        val blocked = ArrayList<FloatArray>()
        padHost?.occupiedRects(blocked)
        val m = LinkUi.dp(this, 4f).toFloat()
        // the menu button goes first, anywhere on the screen (its own corner); the chips then avoid it too
        if (padHost?.editing == true) menuBtn.visibility = View.GONE else {
            menuBtn.visibility = View.VISIBLE
            val d = resources.displayMetrics.density
            val s = GameMenuLogic.BUTTON_DP * d
            val mp = GameMenuLogic.buttonPos(curW.toFloat(), curH.toFloat(), d, blocked)
            val lp = menuBtn.layoutParams as FrameLayout.LayoutParams
            if (lp.leftMargin != mp[0].toInt() || lp.topMargin != mp[1].toInt()) {
                lp.gravity = Gravity.TOP or Gravity.START; lp.leftMargin = mp[0].toInt(); lp.topMargin = mp[1].toInt()
                menuBtn.layoutParams = lp
            }
            blocked.add(floatArrayOf(mp[0] - m, mp[1] - m, mp[0] + s + m, mp[1] + s + m))
        }
        place(onPcPill, area, blocked, m, Placement.PREFER_TOP_LEFT)
        place(sendBtn, area, blocked, m, Placement.PREFER_TOP_RIGHT)
    }

    private fun place(v: View, area: FloatArray, blocked: List<FloatArray>, margin: Float, order: IntArray) {
        if (v.visibility != View.VISIBLE) return
        v.measure(View.MeasureSpec.makeMeasureSpec(curW, View.MeasureSpec.AT_MOST), View.MeasureSpec.makeMeasureSpec(curH, View.MeasureSpec.AT_MOST))
        val p = Placement.pick(area, v.measuredWidth.toFloat(), v.measuredHeight.toFloat(), margin, blocked, order)
        val lp = v.layoutParams as FrameLayout.LayoutParams
        val l = p[0].toInt(); val t = p[1].toInt()
        if (lp.gravity != (Gravity.TOP or Gravity.START) || lp.leftMargin != l || lp.topMargin != t || lp.rightMargin != 0) {
            lp.gravity = Gravity.TOP or Gravity.START
            lp.leftMargin = l; lp.topMargin = t; lp.rightMargin = 0
            v.layoutParams = lp
        }
    }

    @Suppress("DEPRECATION")
    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        }
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
        LinkClient.onForeground(this)
        if (sha1 != null && padHost?.editing != true && !menuPaused) Native.start()
        ui.post(poll)
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        when {
            settingsPanel.shown -> { settingsPanel.hide(); resumeFromMenu() }
            menuSheet.shown -> menuSheet.hide()
            padHost?.exitEdit() != true -> @Suppress("DEPRECATION") super.onBackPressed()
        }
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(poll)
        LinkClient.onBackground()
        Native.stop()
        Native.setKeys(0)
        LinkClient.sendKeys(0)   // release anything the PC may still hold
        // Through the sync engine: state + .sav, new version, atomic. Skipped (1) when nothing changed or the
        // game is being played on the PC (that copy is not ours to overwrite). Never crash on exit.
        if (sha1 != null) Native.saveNow()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) Native.destroy()
    }

    override fun surfaceCreated(holder: SurfaceHolder) { Native.setSurface(holder.surface) }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) { Native.setSurface(null) }

    companion object {
        const val EXTRA_SHA1 = "sha1"
        private const val GBA_W = 240; private const val GBA_H = 160
        private const val POLL_MS = 300L
    }
}

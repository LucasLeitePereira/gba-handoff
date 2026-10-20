package io.gbahandoff

import android.app.Activity
import android.os.Bundle
import android.os.Build
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.FrameLayout
import android.widget.Toast

/**
 * Game screen. Lifecycle: load ROM (+ resume state/sav) in onCreate; emulation runs only between
 * onResume and onPause; onPause also writes state + .sav to internal storage (auto-save on exit).
 */
class GameActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var store: SaveStore
    private lateinit var surface: SurfaceView
    private lateinit var root: FrameLayout
    private var sha1: String? = null
    private var padHost: PadHost? = null
    private var curW = 0
    private var curH = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = SaveStore(filesDir.resolve("games"))
        root = FrameLayout(this).apply { setBackgroundColor(0xFF000000.toInt()) }
        surface = SurfaceView(this)
        surface.holder.addCallback(this)
        root.addView(surface, FrameLayout.LayoutParams(GBA_W, GBA_H, Gravity.TOP or Gravity.CENTER_HORIZONTAL))
        padHost = PadHost(this, { Native.setKeys(it) }) { editing ->   // pad layout editor: pause while editing
            if (editing) Native.stop() else if (sha1 != null) Native.start()
        }
        root.addView(padHost, FrameLayout.LayoutParams(-1, -1))
        root.addOnLayoutChangeListener { _, l, t, r, b, _, _, _, _ -> layoutScreen(r - l, b - t) }
        setContentView(root)
        hideSystemBars()

        if (!Native.create()) return fail("Falha ao criar o core")
        val uri = intent.data ?: return fail("Nenhuma ROM")
        val rc = try {
            contentResolver.openFileDescriptor(uri, "r")!!.use { Native.loadRomFd(it.fd) }
        } catch (e: Exception) { -1000 }
        if (rc != 0) return fail("ROM inválida ($rc)")
        val h = Native.romSha1()!!
        sha1 = h
        resume(h)
    }

    /** Continue where the player left off: state (+ sav) if any, else just the .sav. */
    private fun resume(h: String) {
        val sav = if (store.hasSav(h)) store.savPath(h) else null
        if (store.hasState(h) && Native.restoreFiles(store.statePath(h), sav) == 0) return
        if (sav != null) Native.loadSavFile(sav)
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
        if (sha1 != null && padHost?.editing != true) Native.start()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (padHost?.exitEdit() != true) @Suppress("DEPRECATION") super.onBackPressed()
    }

    override fun onPause() {
        super.onPause()
        LinkClient.onBackground()
        Native.stop()
        Native.setKeys(0)
        sha1?.let { h ->
            // Native.* return codes: log-worthy but never crash on exit.
            Native.saveStateFile(store.statePath(h))
            Native.saveSavFile(store.savPath(h))
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) Native.destroy()
    }

    override fun surfaceCreated(holder: SurfaceHolder) { Native.setSurface(holder.surface) }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) { Native.setSurface(null) }

    private companion object { const val GBA_W = 240; const val GBA_H = 160 }
}

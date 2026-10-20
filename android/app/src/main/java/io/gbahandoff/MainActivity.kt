package io.gbahandoff

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/** ROM picker (SAF) + "continue last game". */
class MainActivity : Activity() {
    private lateinit var cont: Button
    private lateinit var linkText: TextView
    private val ui = Handler(Looper.getMainLooper())
    private val refresh = object : Runnable {
        override fun run() {
            linkText.text = when (LinkClient.status()) {
                LinkClient.CONNECTED -> "Conectado ao PC" + (LinkClient.peerName()?.let { ": $it" } ?: "")
                LinkClient.REFUSED -> "PC recusou: versão do emulador incompatível"
                else -> "Procurando o PC (cabo USB + adb)..."
            }
            ui.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (24 * resources.displayMetrics.density).toInt()
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(pad, pad, pad, pad)
        }
        col.addView(TextView(this).apply { text = getString(R.string.app_name); textSize = 28f; gravity = Gravity.CENTER })
        linkText = TextView(this).apply { gravity = Gravity.CENTER; setPadding(0, pad / 2, 0, pad / 2) }
        col.addView(linkText)
        col.addView(Button(this).apply {
            text = "Escolher ROM (.gba)"
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
                }, REQ_ROM)
            }
        })
        cont = Button(this).apply {
            setOnClickListener { lastRom()?.let { play(it) } }
        }
        col.addView(cont)
        setContentView(col)
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(refresh)
        LinkClient.onBackground()
    }

    override fun onResume() {
        super.onResume()
        LinkClient.onForeground(this)
        ui.post(refresh)
        val last = lastRom()
        cont.visibility = if (last != null) android.view.View.VISIBLE else android.view.View.GONE
        cont.text = "Continuar: " + (last?.lastPathSegment?.substringAfterLast('/') ?: "")
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode == REQ_ROM && resultCode == RESULT_OK && uri != null) {
            try {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (_: SecurityException) { /* provider without persistable grants: still works this session */ }
            getSharedPreferences("app", MODE_PRIVATE).edit().putString("last_rom", uri.toString()).apply()
            play(uri)
        }
    }

    private fun lastRom(): Uri? =
        getSharedPreferences("app", MODE_PRIVATE).getString("last_rom", null)?.let(Uri::parse)

    private fun play(uri: Uri) {
        startActivity(Intent(this, GameActivity::class.java).setData(uri))
    }

    private companion object { const val REQ_ROM = 1 }
}

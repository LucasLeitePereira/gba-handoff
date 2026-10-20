package io.gbahandoff

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.security.SecureRandom

/**
 * Cable link to the PC (TCP to 127.0.0.1:27183 through `adb reverse`). The client thread is
 * native (link_jni.c) and only runs while the app is in the foreground: each Activity calls
 * [onForeground] in onResume and [onBackground] in onPause. The stop is delayed a little so
 * moving between our own Activities does not tear the connection down and redo the HELLO.
 */
object LinkClient {
    // Keep in sync with link_jni.c
    const val STOPPED = 0
    const val SEARCHING = 1
    const val CONNECTED = 2
    const val REFUSED = 3

    private const val STOP_DELAY_MS = 1500L
    private val handler = Handler(Looper.getMainLooper())
    private val stopTask = Runnable { nativeStop() }
    private var started = false

    init { System.loadLibrary("gbahandoff") }

    @JvmStatic private external fun nativeStart(deviceId: ByteArray, name: String)
    @JvmStatic private external fun nativeStop()
    @JvmStatic private external fun nativeStatus(): Int
    @JvmStatic private external fun nativePeerName(): String?

    fun onForeground(ctx: Context) {
        handler.removeCallbacks(stopTask)
        // start is a no-op while the native thread is still running (stop was cancelled)
        nativeStart(deviceId(ctx), deviceName())
        started = true
    }

    fun onBackground() {
        if (started) handler.postDelayed(stopTask, STOP_DELAY_MS)
    }

    fun status(): Int = nativeStatus()
    fun peerName(): String? = nativePeerName()

    /** Stable random id of this installation (16 bytes), created on first use. */
    private fun deviceId(ctx: Context): ByteArray {
        val prefs = ctx.getSharedPreferences("app", Context.MODE_PRIVATE)
        val saved = prefs.getString("device_id", null)
        if (saved != null && saved.length == 32) {
            try { return ByteArray(16) { saved.substring(it * 2, it * 2 + 2).toInt(16).toByte() } } catch (_: NumberFormatException) { }
        }
        val id = ByteArray(16).also { SecureRandom().nextBytes(it) }
        prefs.edit().putString("device_id", id.joinToString("") { "%02x".format(it) }).apply()
        return id
    }

    private fun deviceName(): String =
        if (Build.MODEL.startsWith(Build.MANUFACTURER, ignoreCase = true)) Build.MODEL
        else "${Build.MANUFACTURER} ${Build.MODEL}"
}

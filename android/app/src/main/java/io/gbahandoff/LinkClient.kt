package io.gbahandoff

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.io.File
import java.nio.ByteBuffer
import java.security.SecureRandom

/**
 * Cable link to the PC (TCP to 127.0.0.1:27183 through `adb reverse`). All the logic is native
 * (phone_link.c: session, sync decisions, store, autosave); this object creates the engine once
 * per process, starts/stops its link thread with the foreground, gives it the ROM table, and
 * exposes its state as text ([state], parsed by [LinkState]).
 *
 * The stop is delayed a little so moving between our own Activities does not tear the connection
 * down and redo the HELLO.
 */
object LinkClient {
    // Keep in sync with phone_link.h
    const val STOPPED = 0
    const val SEARCHING = 1
    const val CONNECTED = 2
    const val REFUSED = 3

    private const val STOP_DELAY_MS = 1500L
    private val handler = Handler(Looper.getMainLooper())
    private val stopTask = Runnable { nativeStop() }
    private var started = false
    private var appCtx: Context? = null
    private var roms: RomLibrary? = null

    init { System.loadLibrary("gbahandoff") }

    @JvmStatic private external fun nativeInit(gamesDir: String, romsDir: String, id: ByteArray, name: String)
    @JvmStatic private external fun nativeStart()
    @JvmStatic private external fun nativeStop()
    @JvmStatic private external fun nativeStatus(): Int
    @JvmStatic private external fun nativeSetRoms(shas: Array<String>, sizes: LongArray, sources: Array<String>)
    @JvmStatic private external fun nativeUiText(): String?
    @JvmStatic private external fun nativeConflictThumb(sha: String, remote: Boolean): ByteArray?
    @JvmStatic private external fun nativeReturn()
    @JvmStatic private external fun nativeHandoff()
    @JvmStatic private external fun nativeResolve(sha: String, keepPhone: Boolean)
    @JvmStatic private external fun nativeFetch(sha: String)
    @JvmStatic private external fun nativeAckOpen(sha: String)
    @JvmStatic private external fun nativeMigrate(gamesDir: String, backupDir: String, id: ByteArray): Int

    /**
     * Once per process, before anything else touches the store: migrates the pre-sync store (no
     * progress lost: a copy goes to legacy-backup/), creates the engine and gives it the ROM table.
     */
    @Synchronized fun boot(ctx: Context): RomLibrary {
        roms?.let { return it }
        val app = ctx.applicationContext
        appCtx = app
        val games = File(app.filesDir, "games").also { it.mkdirs() }
        val romsDir = File(app.filesDir, "roms").also { it.mkdirs() }
        val id = deviceId(app)
        val lib = RomLibrary(File(app.filesDir, "roms.tsv"), romsDir)
        roms = lib
        try { nativeMigrate(games.path, File(app.filesDir, "legacy-backup").path, id) } catch (_: Throwable) { }
        nativeInit(games.path, romsDir.path, id, deviceName())
        registerLegacyRom(app, lib)
        publishRoms()
        return lib
    }

    /** Re-sends the ROM table to the engine (after picking a ROM, receiving one...). */
    fun publishRoms() {
        val l = roms?.all() ?: return
        nativeSetRoms(Array(l.size) { l[it].sha1 }, LongArray(l.size) { l[it].size }, Array(l.size) { l[it].source })
    }

    /** The pre-sync app only remembered the last picked ROM's URI: hash it in the background and register it. */
    private fun registerLegacyRom(app: Context, lib: RomLibrary) {
        val prefs = app.getSharedPreferences("app", Context.MODE_PRIVATE)
        val uri = prefs.getString("last_rom", null) ?: return
        if (prefs.getString("last_rom_registered", null) == uri) return
        Thread {
            val rom = hashRom(app, uri) ?: return@Thread
            lib.register(rom)
            prefs.edit().putString("last_rom_registered", uri).apply()
            publishRoms()
        }.also { it.isDaemon = true }.start()
    }

    /** SHA-1 + title of a ROM source (content:// URI or path); null if unreadable. */
    fun hashRom(ctx: Context, source: String): RomLibrary.Rom? {
        val md = java.security.MessageDigest.getInstance("SHA-1")
        val head = ByteArray(0xAC)
        var size = 0L
        try {
            openSource(ctx, source)?.use { ins ->
                val buf = ByteArray(64 * 1024)
                while (true) {
                    val n = ins.read(buf)
                    if (n < 0) break
                    if (size < head.size) System.arraycopy(buf, 0, head, size.toInt(), minOf(n, head.size - size.toInt()))
                    md.update(buf, 0, n); size += n
                }
            } ?: return null
        } catch (_: Exception) { return null }
        if (size < 0xC0) return null
        val sha = md.digest().joinToString("") { "%02x".format(it) }
        return RomLibrary.Rom(sha, size, source, RomLibrary.headerTitle(head))
    }

    fun openSource(ctx: Context, source: String): java.io.InputStream? =
        if (source.startsWith("content://")) ctx.contentResolver.openInputStream(Uri.parse(source))
        else File(source.removePrefix("file://")).inputStream()

    /** Called from the native link thread (JNI): a readable fd of a ROM source that is not a plain file. */
    @JvmStatic fun openRomFd(source: String): Int {
        val ctx = appCtx ?: return -1
        return try {
            ctx.contentResolver.openFileDescriptor(Uri.parse(source), "r")?.detachFd() ?: -1
        } catch (_: Exception) { -1 }
    }

    fun onForeground(ctx: Context) {
        boot(ctx)
        handler.removeCallbacks(stopTask)
        nativeStart()           // no-op while the native thread is still running (stop was cancelled)
        started = true
    }

    fun onBackground() {
        if (started) handler.postDelayed(stopTask, STOP_DELAY_MS)
    }

    fun status(): Int = nativeStatus()
    /** Parsed engine state; notices in it are consumed, so ONE poller per screen should read it. */
    fun state(): LinkState = LinkState.parse(if (roms != null) nativeUiText() else null)

    fun doReturn() = nativeReturn()
    fun sendToPc() = nativeHandoff()
    fun resolve(sha1: String, keepPhone: Boolean) = nativeResolve(sha1, keepPhone)
    fun fetch(sha1: String) = nativeFetch(sha1)
    fun ackOpen(sha1: String) = nativeAckOpen(sha1)

    /** RGBA picture of a conflict side (240x160) or null. */
    fun conflictThumb(sha1: String, remote: Boolean): Bitmap? {
        val px = nativeConflictThumb(sha1, remote) ?: return null
        if (px.size != 240 * 160 * 4) return null
        return Bitmap.createBitmap(240, 160, Bitmap.Config.ARGB_8888).also { it.copyPixelsFromBuffer(ByteBuffer.wrap(px)) }
    }

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

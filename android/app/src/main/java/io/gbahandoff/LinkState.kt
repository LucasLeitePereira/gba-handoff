package io.gbahandoff

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/** One open version conflict (record `C` of phone_link.h). dev kinds: 0 this phone, 1 the PC, 2 other. */
data class Conflict(
    val sha1: String,
    val localTs: Long, val localFrame: Long, val localDev: Int,
    val remoteTs: Long, val remoteFrame: Long, val remoteDev: Int,
    val remoteThumb: Boolean, val waitingPc: Boolean, val haveRemote: Boolean,
)

/** What the native engine reports (phone_ui_text), parsed. Pure JVM. */
data class LinkState(
    val status: Int = LinkClient.STOPPED,
    val peer: String = "",
    /** Games handed to the PC and being played there. */
    val onPc: Set<String> = emptySet(),
    /** Handoff in progress (waiting for the PC's answer). */
    val waiting: Set<String> = emptySet(),
    val pcPlaying: String? = null,
    /** The PC gave this game back and it is not the open one: open it. */
    val openRequest: String? = null,
    /** ROM being received: sha1 -> percent. */
    val receiving: Map<String, Int> = emptyMap(),
    val conflicts: List<Conflict> = emptyList(),
    val notices: List<String> = emptyList(),
) {
    companion object {
        private fun isSha(s: String?) = s != null && s.length == 40 && s.all { it in '0'..'9' || it in 'a'..'f' }

        /** Tolerant: unknown or malformed records are skipped. Notices are consumed by the engine on read. */
        fun parse(text: String?): LinkState {
            if (text.isNullOrEmpty()) return LinkState()
            var st = LinkState()
            val onPc = linkedSetOf<String>(); val waiting = linkedSetOf<String>()
            val rx = linkedMapOf<String, Int>(); val conf = ArrayList<Conflict>(); val notes = ArrayList<String>()
            for (line in text.split('\n')) {
                if (line.length < 2) continue
                val rest = line.substring(2)
                val f = rest.split(' ')
                when (line[0]) {
                    'S' -> {
                        val s = f.firstOrNull()?.toIntOrNull() ?: continue
                        st = st.copy(status = s, peer = rest.substringAfter(' ', ""))
                    }
                    'H' -> if (isSha(rest)) onPc += rest
                    'W' -> if (isSha(rest)) waiting += rest
                    'P' -> if (f.size >= 2 && f[0] == "1" && isSha(f[1])) st = st.copy(pcPlaying = f[1])
                    'O' -> if (isSha(rest)) st = st.copy(openRequest = rest)
                    'T' -> if (f.size >= 2 && isSha(f[0])) rx[f[0]] = (f[1].toIntOrNull() ?: 0).coerceIn(0, 100)
                    'C' -> if (f.size >= 10 && isSha(f[0])) {
                        val n = f.map { it.toLongOrNull() }
                        if (n.subList(1, 10).none { it == null }) conf += Conflict(
                            f[0], n[1]!!, n[2]!!, n[3]!!.toInt(), n[4]!!, n[5]!!, n[6]!!.toInt(),
                            n[7] == 1L, n[8] == 1L, n[9] == 1L)
                    }
                    'N' -> if (rest.isNotBlank()) notes += rest
                }
            }
            return st.copy(onPc = onPc, waiting = waiting, receiving = rx, conflicts = conf, notices = notes)
        }
    }
}

/** Text helpers for the compact UI (pure, testable). */
object Fmt {
    const val GBA_FPS = 59.7275

    fun dateTime(ms: Long, tz: TimeZone = TimeZone.getDefault()): String =
        if (ms <= 0) "—" else SimpleDateFormat("dd/MM HH:mm", Locale.US).also { it.timeZone = tz }.format(Date(ms))

    /** Emulated time of a frame counter: "1h 05m" / "12m 03s" / "45s". */
    fun playTime(frame: Long): String {
        val s = (frame / GBA_FPS).toLong().coerceAtLeast(0)
        return when {
            s >= 3600 -> "%dh %02dm".format(Locale.US, s / 3600, s % 3600 / 60)
            s >= 60 -> "%dm %02ds".format(Locale.US, s / 60, s % 60)
            else -> "${s}s"
        }
    }

    fun device(kind: Int): String = when (kind) { 0 -> "Celular"; 1 -> "PC"; else -> "Outro aparelho" }

    fun shortSha(sha1: String) = sha1.take(8)
}

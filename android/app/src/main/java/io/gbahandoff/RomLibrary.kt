package io.gbahandoff

import java.io.File

/**
 * The ROMs this phone knows: the ones the user picked (SAF URI kept in a small registry file) and
 * the ones received from the PC (<romsDir>/<sha1>.gba). The native engine reads them through the
 * `source` (a content:// URI or an absolute path). Pure JVM (unit-tested).
 */
class RomLibrary(private val registry: File, private val romsDir: File) {
    data class Rom(val sha1: String, val size: Long, val source: String, val title: String)

    private fun clean(s: String) = s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')

    /** Registry + received files; a picked ROM wins over a received copy of the same sha1. */
    @Synchronized fun all(): List<Rom> {
        val out = LinkedHashMap<String, Rom>()
        readRegistry().forEach { out[it.sha1] = it }
        romsDir.listFiles()?.sortedBy { it.name }?.forEach { f ->
            val sha = f.name.removeSuffix(".gba")
            if (f.isFile && f.name.endsWith(".gba") && isSha(sha) && sha !in out)
                out[sha] = Rom(sha, f.length(), f.path, titleOf(f))
        }
        return out.values.toList()
    }

    @Synchronized fun find(sha1: String): Rom? = all().firstOrNull { it.sha1 == sha1 }

    /** Adds or replaces (same sha1) an entry; atomic file replace. */
    @Synchronized fun register(rom: Rom) {
        val list = readRegistry().filter { it.sha1 != rom.sha1 } + rom
        registry.parentFile?.mkdirs()
        val tmp = File(registry.path + ".tmp")
        tmp.writeText(list.joinToString("") { "${it.sha1}\t${it.size}\t${clean(it.source)}\t${clean(it.title)}\n" })
        if (!tmp.renameTo(registry)) { registry.delete(); tmp.renameTo(registry) }
    }

    private fun readRegistry(): List<Rom> {
        if (!registry.isFile) return emptyList()
        return try {
            registry.readLines().mapNotNull { l ->
                val f = l.split('\t')
                if (f.size >= 4 && isSha(f[0]) && f[1].toLongOrNull() != null) Rom(f[0], f[1].toLong(), f[2], f[3]) else null
            }
        } catch (_: Exception) { emptyList() }
    }

    companion object {
        fun isSha(s: String) = s.length == 40 && s.all { it in '0'..'9' || it in 'a'..'f' }

        /** Game title from the cartridge header (12 ASCII bytes at 0xA0). */
        fun headerTitle(head: ByteArray): String {
            if (head.size < 0xAC) return ""
            val sb = StringBuilder()
            for (i in 0xA0 until 0xAC) {
                val c = head[i].toInt() and 0xFF
                if (c == 0) break
                sb.append(if (c in 32..126) c.toChar() else '?')
            }
            return sb.toString().trim()
        }

        fun titleOf(f: File): String = try {
            f.inputStream().use { ins -> ByteArray(0xAC).also { var n = 0; while (n < it.size) { val r = ins.read(it, n, it.size - n); if (r < 0) break; n += r } } }
                .let(::headerTitle)
        } catch (_: Exception) { "" }
    }
}

/** A row of the game list. */
data class GameEntry(
    val sha1: String, val title: String, val hasRom: Boolean,
    /** mtime of the stored progress (ms), 0 = none. */
    val progressMs: Long,
    val onPc: Boolean, val conflict: Boolean, val receivingPct: Int?,
)

object GameList {
    /** Registry ∪ store, newest progress first; the state decides what is on the PC / in conflict. */
    fun build(roms: List<RomLibrary.Rom>, gamesDir: File, state: LinkState): List<GameEntry> {
        val byTitle = roms.associateBy { it.sha1 }
        val shas = LinkedHashSet<String>()
        roms.forEach { shas += it.sha1 }
        gamesDir.listFiles()?.forEach { f ->
            if (f.name.endsWith(".state") && RomLibrary.isSha(f.name.removeSuffix(".state"))) shas += f.name.removeSuffix(".state")
        }
        state.receiving.keys.forEach { shas += it }
        return shas.map { sha ->
            val rom = byTitle[sha]
            val st = File(gamesDir, "$sha.state")
            GameEntry(
                sha, rom?.title?.takeIf { it.isNotBlank() } ?: "Jogo ${Fmt.shortSha(sha)}", rom != null,
                if (st.isFile) st.lastModified() else 0L,
                sha in state.onPc, state.conflicts.any { it.sha1 == sha }, state.receiving[sha],
            )
        }.sortedWith(compareByDescending<GameEntry> { it.progressMs }.thenBy { it.title })
    }
}

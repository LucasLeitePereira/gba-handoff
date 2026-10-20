package io.gbahandoff

import java.io.File

/** Per-game files in the app's internal storage, keyed by ROM SHA-1. Pure JVM (unit-tested). */
class SaveStore(private val dir: File) {
    init { dir.mkdirs() }

    fun statePath(sha1: String) = File(dir, "${safe(sha1)}.state").path
    fun savPath(sha1: String) = File(dir, "${safe(sha1)}.sav").path
    fun hasState(sha1: String) = File(statePath(sha1)).isFile
    fun hasSav(sha1: String) = File(savPath(sha1)).isFile

    private fun safe(sha1: String): String {
        require(sha1.length == 40 && sha1.all { it in '0'..'9' || it in 'a'..'f' }) { "bad sha1" }
        return sha1
    }
}

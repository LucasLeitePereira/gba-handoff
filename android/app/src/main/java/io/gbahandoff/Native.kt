package io.gbahandoff

import android.view.Surface

/** JNI bridge to jni_bridge.c. One emulator instance per process. */
object Native {
    init { System.loadLibrary("gbahandoff") }

    /** Key bits = GBA KEYINPUT order (same as core/gbah_core.h and the future wire format). */
    const val KEY_A = 1 shl 0
    const val KEY_B = 1 shl 1
    const val KEY_SELECT = 1 shl 2
    const val KEY_START = 1 shl 3
    const val KEY_RIGHT = 1 shl 4
    const val KEY_LEFT = 1 shl 5
    const val KEY_UP = 1 shl 6
    const val KEY_DOWN = 1 shl 7
    const val KEY_R = 1 shl 8
    const val KEY_L = 1 shl 9

    @JvmStatic external fun create(): Boolean
    @JvmStatic external fun destroy()
    @JvmStatic external fun loadRomBytes(rom: ByteArray): Int
    /** Reads the fd in native memory; does not close it. */
    @JvmStatic external fun loadRomFd(fd: Int): Int
    @JvmStatic external fun romSha1(): String?
    /** Cold start with an existing .sav, before running. */
    @JvmStatic external fun loadSavFile(path: String): Int
    /** Resume = state + (optional) sav, applied in the right order. */
    @JvmStatic external fun restoreFiles(statePath: String, savPath: String?): Int
    @JvmStatic external fun restore(state: ByteArray, sav: ByteArray?): Int
    @JvmStatic external fun exportState(): ByteArray?
    @JvmStatic external fun exportSav(): ByteArray?
    @JvmStatic external fun saveStateFile(path: String): Int
    /** 0 ok, 1 = game has no save data yet (nothing written). */
    @JvmStatic external fun saveSavFile(path: String): Int
    @JvmStatic external fun setSurface(surface: Surface?)
    @JvmStatic external fun setKeys(keys: Int)
    @JvmStatic external fun start(): Boolean
    @JvmStatic external fun stop()
    @JvmStatic external fun frameCounter(): Int
}

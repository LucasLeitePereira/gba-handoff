package io.gbahandoff

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class RomLibraryTest {
    @get:Rule val tmp = TemporaryFolder()
    private val a = "a".repeat(40)
    private val b = "b".repeat(40)
    private val c = "c".repeat(40)

    private fun header(title: String): ByteArray = ByteArray(0xC0).also { title.toByteArray().copyInto(it, 0xA0) }
    private fun lib() = RomLibrary(File(tmp.root, "roms.tsv"), File(tmp.root, "roms"))

    @Test fun headerTitle() {
        assertEquals("POKEMON EMER", RomLibrary.headerTitle(header("POKEMON EMER")))
        assertEquals("", RomLibrary.headerTitle(ByteArray(10)))
        assertEquals("A?B", RomLibrary.headerTitle(ByteArray(0xC0).also { it[0xA0] = 'A'.code.toByte(); it[0xA1] = 0x80.toByte(); it[0xA2] = 'B'.code.toByte() }))
    }

    @Test fun registerPersistsAndReplaces() {
        val l = lib()
        l.register(RomLibrary.Rom(a, 100, "content://x/1", "UM"))
        l.register(RomLibrary.Rom(b, 200, "/p/b.gba", "DOIS"))
        l.register(RomLibrary.Rom(a, 150, "content://x/2", "UM v2"))      // same sha1: replaced
        val again = lib().all()                                           // a new instance reads the file
        assertEquals(listOf(b, a), again.map { it.sha1 })
        assertEquals(RomLibrary.Rom(a, 150, "content://x/2", "UM v2"), lib().find(a))
        assertNull(lib().find(c))
    }

    @Test fun separatorsInTitleDoNotBreakTheFile() {
        val l = lib()
        l.register(RomLibrary.Rom(a, 1, "content://a\tb", "tí\ttulo\nx"))
        val r = lib().find(a)
        assertNotNull(r)
        assertEquals("content://a b", r!!.source)
        assertEquals("tí tulo x", r.title)
    }

    @Test fun corruptLinesAreIgnored() {
        File(tmp.root, "roms.tsv").writeText("lixo\n$a\tnan\tx\ty\n$b\t5\tsrc\tTIT\n\n")
        assertEquals(listOf(b), lib().all().map { it.sha1 })
    }

    @Test fun receivedRomsAreListedFromTheRomsDir() {
        val dir = File(tmp.root, "roms").also { it.mkdirs() }
        File(dir, "$c.gba").writeBytes(header("RECEBIDA"))
        File(dir, "naosha.gba").writeBytes(header("X"))
        File(dir, "$b.txt").writeText("x")
        val l = lib()
        l.register(RomLibrary.Rom(a, 9, "content://p", "ESCOLHIDA"))
        val all = l.all()
        assertEquals(setOf(a, c), all.map { it.sha1 }.toSet())
        val rec = l.find(c)!!
        assertEquals("RECEBIDA", rec.title)
        assertEquals(File(dir, "$c.gba").path, rec.source)
        assertEquals(0xC0L, rec.size)
    }

    @Test fun pickedRomWinsOverReceivedCopy() {
        val dir = File(tmp.root, "roms").also { it.mkdirs() }
        File(dir, "$a.gba").writeBytes(header("COPIA"))
        val l = lib()
        l.register(RomLibrary.Rom(a, 9, "content://p", "ESCOLHIDA"))
        assertEquals("content://p", l.find(a)!!.source)
    }

    @Test fun isSha() {
        assertTrue(RomLibrary.isSha(a))
        assertFalse(RomLibrary.isSha("A".repeat(40)))
        assertFalse(RomLibrary.isSha("abc"))
    }
}

class GameListTest {
    @get:Rule val tmp = TemporaryFolder()
    private val a = "a".repeat(40)   // legacy save (the Pokémon one): state+sav, no ROM registered yet
    private val b = "b".repeat(40)   // ROM known, never played
    private val c = "c".repeat(40)   // being received

    @Test fun legacyStoreShowsUpWithItsProgress() {
        val games = tmp.newFolder("games")
        File(games, "$a.state").also { it.writeBytes(ByteArray(10)); it.setLastModified(1_700_000_000_000) }
        File(games, "$a.sav").writeBytes(ByteArray(10))
        File(games, "$a.meta").writeBytes(ByteArray(128))      // written by the migration: must not create a second row
        val list = GameList.build(listOf(RomLibrary.Rom(b, 1, "s", "NOVO")), games, LinkState())
        assertEquals(listOf(a, b), list.map { it.sha1 })
        assertEquals(1_700_000_000_000, list[0].progressMs)
        assertFalse(list[0].hasRom)
        assertTrue(list[0].title.startsWith("Jogo aaaaaaaa"))
        assertEquals(0L, list[1].progressMs)
        assertTrue(list[1].hasRom)
    }

    @Test fun statusFlagsComeFromTheLinkState() {
        val games = tmp.newFolder("games2")
        val roms = listOf(a, b).map { RomLibrary.Rom(it, 1, "s", it.take(1).uppercase()) }
        val st = LinkState(
            onPc = setOf(a), receiving = mapOf(c to 30),
            conflicts = listOf(Conflict(b, 0, 0, 0, 0, 0, 1, false, true, false)),
        )
        val m = GameList.build(roms, games, st).associateBy { it.sha1 }
        assertTrue(m[a]!!.onPc); assertFalse(m[a]!!.conflict)
        assertTrue(m[b]!!.conflict)
        assertEquals(30, m[c]!!.receivingPct)
        assertFalse(m[c]!!.hasRom)
    }

    @Test fun newestProgressFirst() {
        val games = tmp.newFolder("games3")
        File(games, "$a.state").also { it.writeBytes(ByteArray(1)); it.setLastModified(1_000_000) }
        File(games, "$b.state").also { it.writeBytes(ByteArray(1)); it.setLastModified(2_000_000) }
        assertEquals(listOf(b, a), GameList.build(emptyList(), games, LinkState()).map { it.sha1 })
    }
}

package io.gbahandoff

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SaveStoreTest {
    @get:Rule val tmp = TemporaryFolder()
    private val sha = "0123456789abcdef0123456789abcdef01234567"

    @Test fun pathsAreKeyedBySha1AndStartMissing() {
        val s = SaveStore(tmp.root.resolve("g"))
        assertTrue(s.statePath(sha).endsWith("$sha.state"))
        assertTrue(s.savPath(sha).endsWith("$sha.sav"))
        assertFalse(s.hasState(sha))
        java.io.File(s.savPath(sha)).writeText("x")
        assertTrue(s.hasSav(sha))
    }

    @Test fun rejectsPathTraversal() {
        val s = SaveStore(tmp.root)
        assertThrows(IllegalArgumentException::class.java) { s.statePath("../../etc/passwd") }
    }
}

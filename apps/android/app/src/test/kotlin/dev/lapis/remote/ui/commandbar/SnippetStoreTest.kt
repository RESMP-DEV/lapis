package dev.lapis.remote.ui.commandbar

import dev.lapis.remote.platform.KeyValueStore
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlinx.coroutines.test.runTest

private class MemoryStore : KeyValueStore {
    val values = mutableMapOf<String, String>()
    override suspend fun getString(key: String): String? = values[key]
    override suspend fun putString(key: String, value: String) {
        values[key] = value
    }
}

/** The per-device snippet list under one settings key. */
class SnippetStoreTest {

    @Test
    fun emptyWhenNeverSaved() = runTest {
        val store = SnippetStore(MemoryStore())
        assertTrue(store.load().isEmpty())
    }

    @Test
    fun roundTripsOrderAndText() = runTest {
        val memory = MemoryStore()
        val store = SnippetStore(memory)
        store.save(listOf("git status", "count", "size probe"))
        assertEquals(listOf("git status", "count", "size probe"), store.load())
        assertEquals("commandSnippets", memory.values.keys.single())
    }

    @Test
    fun garbageDecodesToEmptyInsteadOfCrashing() = runTest {
        val memory = MemoryStore()
        memory.values[SnippetStore.KEY] = "{\"snippets\": true"
        assertTrue(SnippetStore(memory).load().isEmpty())
    }

    @Test
    fun wrongShapeDecodesToEmpty() = runTest {
        val memory = MemoryStore()
        memory.values[SnippetStore.KEY] = "[1, 2, 3]"
        assertTrue(SnippetStore(memory).load().isEmpty())
    }

    @Test
    fun loadTrimsAndDropsBlanksAndCapsTheCount() = runTest {
        val memory = MemoryStore()
        val many = (1..SnippetStore.MAX_SNIPPETS + 5).map { "cmd $it" }
        memory.values[SnippetStore.KEY] =
            "[\"  padded  \", \"\", " + many.joinToString(",") { "\"$it\"" } + "]"
        val loaded = SnippetStore(memory).load()
        assertEquals("padded", loaded.first())
        assertEquals(SnippetStore.MAX_SNIPPETS, loaded.size)
    }

    @Test
    fun saveTrimsDropsBlanksAndCapsTheCount() = runTest {
        val memory = MemoryStore()
        SnippetStore(memory).save(listOf("  a  ", "") + (1..100).map { "s$it" })
        assertEquals(
            listOf("a") + (1..SnippetStore.MAX_SNIPPETS - 1).map { "s$it" },
            SnippetStore(memory).load(),
        )
    }

    @Test
    fun normalizeIsWhatSavePersistsAndLoadReturns() = runTest {
        // The truncation-boundary entry is the non-obvious one: cutting at
        // MAX_LENGTH right before whitespace used to leave a trailing
        // space that a reload's trim would strip, so the live list and the
        // reloaded list disagreed by one character.
        val boundary = "a".repeat(SnippetStore.MAX_LENGTH - 1) + " b"
        val raw = listOf("  a  ", "", boundary) + (1..100).map { "s$it" }
        val memory = MemoryStore()
        SnippetStore(memory).save(raw)
        // The live bar assigns normalize() directly, so this equality is
        // the invariant that display, persistence, and reload agree.
        assertEquals(SnippetStore.normalize(raw), SnippetStore(memory).load())
        // And normalize is idempotent: applying it twice changes nothing,
        // which is what save-then-load amounts to.
        assertEquals(SnippetStore.normalize(raw), SnippetStore.normalize(SnippetStore.normalize(raw)))
    }
}

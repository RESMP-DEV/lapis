package dev.lapis.remote.ui.commandbar

import androidx.compose.runtime.saveable.SaverScope
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull

class SnippetListSaverTest {

    private object AnyScope : SaverScope {
        override fun canBeSaved(value: Any): Boolean = true
    }

    @Test
    fun emptyRowListSurvivesSaveAndRestore() {
        val saved = with(AnyScope) { with(SnippetListSaver) { save(emptyList()) } }
        // Null means "nothing saved": an editor emptied of every row would
        // reset to the committed list on recreation and lose the deletions.
        // This is exactly what the library listSaver returns for empty lists.
        assertNotNull(saved)
        assertEquals(emptyList(), SnippetListSaver.restore(saved!!))
    }

    @Test
    fun rowListRoundTrips() {
        val original = listOf("git status", "count")
        val saved = with(AnyScope) { with(SnippetListSaver) { save(original) } }
        assertEquals(original, SnippetListSaver.restore(saved!!))
    }
}

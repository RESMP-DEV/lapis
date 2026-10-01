package dev.lapis.remote.ui.commandbar

import androidx.compose.runtime.saveable.SaverScope
import androidx.compose.runtime.saveable.autoSaver
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

    @Test
    fun plainAutoSaverAlsoPreservesEmptyLists() {
        // The null-for-empty drop is specific to the library's *builder*
        // savers (listSaver/mapSaver), which is why the editor's row list
        // needs the explicit SnippetListSaver. The plain autoSaver that
        // rememberSaveable uses for a List state without a stateSaver
        // saves the value itself, so an empty list round-trips without
        // help: the stage-level `snippets` state needs no explicit Saver.
        val saver = autoSaver<List<String>>()
        val saved = with(AnyScope) { with(saver) { save(emptyList()) } }
        assertNotNull(saved)
        assertEquals(emptyList(), saver.restore(saved!!))
    }
}

package dev.lapis.remote.ui.commandbar

import dev.lapis.remote.platform.KeyValueStore
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/**
 * The per-device snippet list: one JSON array of strings under a single
 * DataStore key, never synced through the gateway. Decode failures fall
 * back to empty rather than crashing the stage; loads and saves are
 * bounded so the file cannot grow without limit.
 */
class SnippetStore(private val store: KeyValueStore) {

    suspend fun load(): List<String> {
        val raw = store.getString(KEY) ?: return emptyList()
        val decoded = try {
            json.decodeFromString<List<String>>(raw)
        } catch (_: IllegalArgumentException) {
            return emptyList()
        }
        return decoded.map { it.trim() }.filter { it.isNotEmpty() }.take(MAX_SNIPPETS)
    }

    suspend fun save(snippets: List<String>) {
        val bounded = snippets
            .map { it.trim().take(MAX_LENGTH) }
            .filter { it.isNotEmpty() }
            .take(MAX_SNIPPETS)
        store.putString(KEY, json.encodeToString(bounded))
    }

    companion object {
        /** DataStore key, matching the iOS app's "commandSnippets". */
        const val KEY = "commandSnippets"
        const val MAX_SNIPPETS = 24
        const val MAX_LENGTH = 512
        private val json = Json { ignoreUnknownKeys = true }
    }
}

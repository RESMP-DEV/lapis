package dev.lapis.remote.ui.commandbar

import dev.lapis.remote.platform.KeyValueStore
import kotlinx.serialization.SerializationException
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.serializer
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
        // SerializationException is the decode failure this store can hit
        // (malformed JSON, wrong element shapes); naming it instead of its
        // IllegalArgumentException supertype keeps the catch from also
        // absorbing an unrelated argument bug as "corrupt data".
        val decoded = try {
            json.decodeFromString(ListSerializer(String.serializer()), raw)
        } catch (_: SerializationException) {
            return emptyList()
        }
        return normalize(decoded)
    }

    suspend fun save(snippets: List<String>) {
        store.putString(
            KEY,
            json.encodeToString(ListSerializer(String.serializer()), normalize(snippets)),
        )
    }

    companion object {
        /** DataStore key, matching the iOS app's "commandSnippets". */
        const val KEY = "commandSnippets"
        const val MAX_SNIPPETS = 24
        const val MAX_LENGTH = 512
        private val json = Json { ignoreUnknownKeys = true }

        /** The one bounds projection: what load() returns is exactly what
         * save() persists and exactly what the bar displays, so the live
         * chips can never diverge from what a restart restores. The final
         * trimEnd keeps it idempotent: truncation can otherwise cut right
         * before whitespace and leave a tail that a reload would strip. */
        fun normalize(snippets: List<String>): List<String> = snippets
            .map { it.trim().take(MAX_LENGTH).trimEnd() }
            .filter { it.isNotEmpty() }
            .take(MAX_SNIPPETS)
    }
}

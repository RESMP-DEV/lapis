package dev.lapis.remote.platform

import android.content.Context
import androidx.datastore.core.handlers.ReplaceFileCorruptionHandler
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStoreFile
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import dev.lapis.remote.gateway.GatewayError
import java.io.IOException
import javax.net.ssl.SSLException
import kotlinx.coroutines.flow.first

/** Key-value persistence seam; DataStore on device, in-memory fakes in tests. */
interface KeyValueStore {
    suspend fun getString(key: String): String?
    suspend fun putString(key: String, value: String)
}

/** DataStore-backed settings (the Android port of iOS UserDefaults). */
class DataStoreSettings(context: Context) : KeyValueStore {
    // Without the corruption handler, one unreadable file poisons the store
    // forever: every later read and write throws CorruptionException, so
    // persistence stays dead until the app's data is cleared. Replacing the
    // corrupt file with empty preferences loses the stored settings once —
    // the same defaults the absorption below already returns — and the next
    // write recreates the file.
    private val store = PreferenceDataStoreFactory.create(
        produceFile = { context.preferencesDataStoreFile(FILE) },
        corruptionHandler = ReplaceFileCorruptionHandler { emptyPreferences() },
    )

    // UserDefaults is best-effort storage: reads fall back to defaults and
    // writes either land or do not, but neither ever takes the app down.
    // DataStore instead throws IOException/CorruptionException on file
    // errors, and an uncaught exception in a Main-dispatcher coroutine is a
    // process kill — so the seam absorbs storage failures here, once, for
    // every key (host, font size, snippets, command bar).

    override suspend fun getString(key: String): String? = try {
        store.data.first()[stringPreferencesKey(key)]
    } catch (_: IOException) {
        // CorruptionException is an IOException subclass, so this one catch
        // covers both DataStore corruption and file read failures.
        null
    }

    override suspend fun putString(key: String, value: String) {
        try {
            store.edit { preferences -> preferences[stringPreferencesKey(key)] = value }
        } catch (_: IOException) {
        }
    }

    companion object {
        private const val FILE = "lapis-settings"
    }
}

/**
 * Connectivity error mapping in the iOS `describe(_:)` style: transport
 * failures against a quiet Mac get the actionable line; gateway errors carry
 * their own copy; anything else surfaces its message.
 */
fun describe(error: Throwable): String = when (error) {
    is GatewayError -> error.message ?: error.toString()
    is SSLException -> error.message ?: "The secure connection to the Mac failed."
    is IOException -> "The Mac did not answer. Check that Tailscale or ZeroTier is on here and the Mac is awake."
    else -> error.message ?: error.toString()
}

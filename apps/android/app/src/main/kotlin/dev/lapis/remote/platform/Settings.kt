package dev.lapis.remote.platform

import android.content.Context
import androidx.datastore.preferences.core.edit
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
    private val store = PreferenceDataStoreFactory.create(
        produceFile = { context.preferencesDataStoreFile(FILE) },
    )

    override suspend fun getString(key: String): String? =
        store.data.first()[stringPreferencesKey(key)]

    override suspend fun putString(key: String, value: String) {
        store.edit { preferences -> preferences[stringPreferencesKey(key)] = value }
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
    is IOException -> "The Mac did not answer. Check that Tailscale is on here and the Mac is awake."
    else -> error.message ?: error.toString()
}

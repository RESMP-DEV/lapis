package dev.lapis.remote.ui

import android.app.Application
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.repeatOnLifecycle
import dev.lapis.remote.platform.DataStoreSettings
import dev.lapis.remote.session.DiskCache
import dev.lapis.remote.session.RepositoryFactory
import dev.lapis.remote.session.WorkspaceRepository
import java.io.File
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Owns the workspace repository so it survives activity recreation. */
class LapisApplication : Application() {
    val repository: WorkspaceRepository by lazy {
        RepositoryFactory.create(
            store = DataStoreSettings(this),
            cache = DiskCache(File(filesDir, "cache")),
        )
    }
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val repository = (application as LapisApplication).repository
        setContent {
            LapisTheme {
                LapisApp(repository)
            }
        }
    }
}

@Composable
private fun LapisApp(repository: WorkspaceRepository) {
    val host by repository.host.collectAsStateWithLifecycle()
    val listing by repository.listing.collectAsStateWithLifecycle()
    val error by repository.error.collectAsStateWithLifecycle()
    val refreshing by repository.refreshing.collectAsStateWithLifecycle()

    var showSettings by rememberSaveable { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val lifecycle = LocalLifecycleOwner.current.lifecycle

    // Restore the persisted host, then keep the list current while the app is
    // in the foreground, mirroring the iOS refresh loop (foreground only; no
    // background network by design). One effect, so the first poll always
    // uses the restored host; polling runs only while the app is started.
    LaunchedEffect(repository) {
        repository.restore()
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            while (isActive) {
                repository.refresh()
                delay(8_000)
            }
        }
    }

    BackHandler(enabled = showSettings) { showSettings = false }

    if (showSettings) {
        SettingsScreen(
            currentHost = host,
            onDone = { saved ->
                showSettings = false
                repository.setHost(saved)
            },
            onCancel = { showSettings = false },
        )
    } else {
        WorkspaceScreen(
            host = host,
            listing = listing,
            error = error,
            refreshing = refreshing,
            onRefresh = { scope.launch { repository.refresh() } },
            onOpenSettings = { showSettings = true },
        )
    }
}

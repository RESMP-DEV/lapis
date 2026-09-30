package dev.lapis.remote.ui

import android.app.Application
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import dev.lapis.remote.platform.DataStoreSettings
import dev.lapis.remote.session.DiskCache
import dev.lapis.remote.session.RepositoryFactory
import dev.lapis.remote.session.ScreenCache
import dev.lapis.remote.session.WorkspaceRepository
import java.io.File
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Owns the workspace repository so it survives activity recreation. */
class LapisApplication : Application() {
    val settings: DataStoreSettings by lazy { DataStoreSettings(this) }
    val repository: WorkspaceRepository by lazy {
        RepositoryFactory.create(
            store = settings,
            cache = DiskCache(File(filesDir, "cache")),
        )
    }
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // First creation only: a recreation must never wipe caches or re-apply.
        if (savedInstanceState == null) applyLaunchOverrides(intent)
        val repository = (application as LapisApplication).repository
        setContent {
            LapisTheme {
                // Expose testTags as resource-ids so adb's uiautomator can
                // find tagged controls; identifiers mirror the iOS app.
                Box(Modifier.semantics { testTagsAsResourceId = true }) {
                    LapisApp(repository)
                }
            }
        }
    }

    /**
     * Debug-build launch overrides, the port of the iOS UI tests'
     * launchArguments (`-gatewayHost`, `-terminalFontSize`, `-resetCache`):
     * `am start --es gatewayHost 127.0.0.1:7351 --ez resetCache true` points a
     * debuggable build at a test gateway without touching the Settings UI.
     * Release builds ignore the extras entirely.
     */
    private fun applyLaunchOverrides(launch: Intent) {
        if ((applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) == 0) return
        val owner = application as LapisApplication
        if (launch.getBooleanExtra("resetCache", false)) {
            // Before the lazy repository ever reads the directory.
            ScreenCache.clearAll()
            File(filesDir, "cache").deleteRecursively()
        }
        launch.getStringExtra("gatewayHost")?.let { host ->
            owner.repository.setHost(host)
        }
        launch.getStringExtra("terminalFontSize")?.toFloatOrNull()?.let { size ->
            lifecycleScope.launch { owner.settings.putString(FONT_SIZE_KEY, size.toString()) }
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
    var openAgentId by rememberSaveable { mutableStateOf<String?>(null) }
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

    BackHandler(enabled = showSettings || openAgentId != null) {
        if (showSettings) showSettings = false else openAgentId = null
    }

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
        // The listing's own copy of the agent wins over a remembered one, so
        // renames and status changes show; the id alone is the identity.
        val openAgent = openAgentId?.let { id ->
            listing?.categories?.flatMap { it.agents }?.firstOrNull { it.id == id }
        }
        if (openAgentId != null && openAgent != null) {
            AgentStage(
                agent = openAgent,
                repository = repository,
                settings = (LocalContext.current.applicationContext as LapisApplication).settings,
                onBack = { openAgentId = null },
            )
        } else {
            WorkspaceScreen(
                host = host,
                listing = listing,
                error = error,
                refreshing = refreshing,
                onRefresh = { scope.launch { repository.refresh() } },
                onOpenSettings = { showSettings = true },
                onOpenAgent = { agent -> openAgentId = agent.id },
            )
        }
    }
}

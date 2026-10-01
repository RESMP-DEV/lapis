package dev.lapis.remote.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dev.lapis.remote.gateway.Agent
import dev.lapis.remote.gateway.Input
import dev.lapis.remote.platform.KeyValueStore
import dev.lapis.remote.session.AgentSession
import dev.lapis.remote.session.WorkspaceRepository
import dev.lapis.remote.terminal.TerminalMetrics
import dev.lapis.remote.terminal.TerminalScreen
import dev.lapis.remote.ui.commandbar.CommandBar
import dev.lapis.remote.ui.commandbar.SnippetListSaver
import dev.lapis.remote.ui.commandbar.SnippetStore
import kotlin.math.max
import kotlin.math.min
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * The agent's stage: the live terminal, the state banner, and the composer.
 * Port of `AgentView` in AgentView.swift (the fixed key row is milestone C;
 * "Send screen to Mac" is milestone E). The keyboard only covers the screen,
 * never the agent: keyboard-only height changes never resize the terminal,
 * while a new width, or a new height with the keyboard down, does.
 */
@OptIn(
    ExperimentalMaterial3Api::class,
    androidx.compose.foundation.layout.ExperimentalLayoutApi::class,
)
@Composable
fun AgentStage(
    agent: Agent,
    repository: WorkspaceRepository,
    settings: KeyValueStore,
    sessionScope: CoroutineScope,
    fontSizeOverride: Float?,
    commandBarOverride: Boolean? = null,
    onBack: () -> Unit,
) {
    val host by repository.host.collectAsStateWithLifecycle()
    val gateway = remember(host) { repository.gatewayFor(host) }
    // An application-lived scope, not rememberCoroutineScope(): that one is
    // cancelled before DisposableEffect.onDispose runs, so the session's jobs
    // would already be dead when close() goes to retire them.
    val session = remember(agent.id, gateway) { AgentSession(agent, gateway, sessionScope) }
    DisposableEffect(session) {
        onDispose { session.close() }
    }
    // The Mac may rename the agent mid-session; the session (keyed by id) stays.
    LaunchedEffect(agent, session) { session.updateAgent(agent) }

    val state by session.state.collectAsStateWithLifecycle()
    val frame by session.frame.collectAsStateWithLifecycle()
    val shared by session.shared.collectAsStateWithLifecycle()
    val history by session.history.collectAsStateWithLifecycle()
    val historyEnd by session.historyEnd.collectAsStateWithLifecycle()
    val loadingHistory by session.loadingHistory.collectAsStateWithLifecycle()
    val notice by session.notice.collectAsStateWithLifecycle()
    val sessionSize by session.size.collectAsStateWithLifecycle()

    var fontSize by remember { mutableStateOf(fontSizeOverride ?: DEFAULT_FONT_SIZE) }

    // The command bar's visibility and snippet list are phone-local state
    // under the same settings seam; the bar is on until turned off. Both
    // are saveable so a fold/unfold (activity recreation) shows the saved
    // values immediately instead of flashing defaults until the DataStore
    // read completes. A debug launch may override the bar's visibility in
    // memory: the device harness always forces the bar on, so a missing
    // bar in a harness run is a regression, never a setting question.
    val snippetStore = remember(settings) { SnippetStore(settings) }
    var commandBarEnabled by rememberSaveable { mutableStateOf(commandBarOverride ?: true) }
    // The editor's Saver, not the default: rememberSaveable's default maps
    // an empty list to Bundle-null and reads null as "no saved value", so a
    // legitimately empty list would not survive a fold (documented at
    // SnippetEditorDialog; internal visibility is module-wide, so the ui
    // package reuses it without hoisting).
    var snippets by rememberSaveable(stateSaver = SnippetListSaver) {
        mutableStateOf(listOf<String>())
    }

    // Saved settings land after first composition: DataStore reads are
    // async where iOS UserDefaults is synchronous. fit() must wait for
    // them, or the session opens at the default font and bar visibility
    // and resizes a moment later when the saved values apply. One effect
    // reads everything and flips the gate once, so the first open happens
    // at the geometry the user actually configured.
    var settingsLoaded by remember { mutableStateOf(false) }
    // Saveable for the same reason as SettingsScreen's loadedOnce: a fold
    // recreates the stage with the list the user just committed (snippets
    // is saveable, and setSnippets marks this true with the commit), so a
    // store read racing that save must not assign the pre-write list over
    // the restored one.
    var snippetsLoaded by rememberSaveable { mutableStateOf(false) }
    // Keyed on settings only: the override is a debug intent, and re-running
    // the store loads when it changes would revert a snippet save still in
    // flight back to the persisted list.
    val currentFontOverride by rememberUpdatedState(fontSizeOverride)
    LaunchedEffect(settings) {
        try {
            if (currentFontOverride == null) {
                fontSize = settings.getString(FONT_SIZE_KEY)?.toFloatOrNull()
                    ?.coerceIn(MIN_FONT_SIZE, MAX_FONT_SIZE) ?: DEFAULT_FONT_SIZE
            }
            commandBarEnabled = commandBarOverride
                ?: settings.getString(COMMAND_BAR_KEY)?.toBooleanStrictOrNull()
                ?: true
            val loaded = snippetStore.load()
            // A recreation re-runs this effect with the saveable list the
            // user last saw (or committed) already restored; assigning the
            // store's pre-write list over it would revert the screen and,
            // once edited, drop the in-flight save underneath.
            if (!snippetsLoaded) {
                snippets = loaded
                snippetsLoaded = true
            }
        } finally {
            // A stage that never opens is a worse failure than one that
            // opens at default geometry: any unexpected throwable in the
            // reads still releases the fit() gate.
            settingsLoaded = true
        }
    }

    val setSnippets: (List<String>) -> Unit = remember(sessionScope, snippetStore) {
        { next: List<String> ->
            // The store's bounds apply to the live list too, so the bar shows
            // exactly what a restart restores.
            val bounded = SnippetStore.normalize(next)
            snippets = bounded
            // The commit makes the live list authoritative: a store read
            // still in flight must not overwrite it after a recreation.
            snippetsLoaded = true
            // The application-lived scope: leaving the stage (or the editor
            // closing) must not cancel the persistence write.
            sessionScope.launch { snippetStore.save(bounded) }
        }
    }

    val density = LocalDensity.current
    val metrics = remember(fontSize, density) {
        TerminalMetrics.system(with(density) { fontSize.sp.toPx() })
    }

    var composing by remember { mutableStateOf(false) }
    var draft by rememberSaveable { mutableStateOf("") }
    var stageSize by remember { mutableStateOf(IntSize.Zero) }
    // The bar renders only once its visibility is settled: an override makes
    // it final at first composition, and otherwise the DataStore read does.
    // Rendering the default first would flash the bar (and its chips) at a
    // user whose saved value is off. The same effect that settles the flag
    // also assigns the loaded snippets, so the bar's first frame carries the
    // real chip row, not an empty one.
    val barShown = (commandBarOverride != null || settingsLoaded) && commandBarEnabled
    // Which bar visibility the current stageSize was measured under — the
    // RENDERED visibility (barShown), not the raw setting. Until the read
    // settles, no bar renders, so every pre-read measurement is a tall
    // one taken under bar-off; recording the raw (default-on) setting
    // here would pair that measurement with a with-bar provenance: a
    // saved-on entry would open tall and resize once the bar lands, and a
    // saved-off entry would never agree (nothing re-measures when the bar
    // never renders), wedging the session closed forever. The initializer
    // mirrors the first frame's rendered state, so the pair starts
    // agreeing in the override cases and never waits on a relayout that
    // will not come.
    var laidOutBarEnabled by remember { mutableStateOf(commandBarOverride == true) }
    val keyboardShown = WindowInsets.isImeVisible
    val composerFocus = remember { FocusRequester() }

    // Stable lambdas so TerminalScreen and Banner stay skippable; a fresh
    // lambda on every composition would recompose them each frame.
    val loadOlder: suspend () -> Unit = remember(session) { { session.loadOlder() } }
    val onReopen = remember(session) { { columns: Int, rows: Int -> session.open(columns, rows) } }

    // Reopen at the geometry the stage has now, not the size the dying
    // attachment last reported: a fold or rotation between close and reopen
    // would otherwise resurrect the stale grid. The keyboard-only rule of
    // fit() applies here too — the IME shrinks the padded stage, and a
    // reopen with the keyboard up must not shrink the row count.
    val reopenGrid: AgentSession.Grid? = remember(stageSize, metrics, keyboardShown, composing, sessionSize) {
        if (stageSize.width > 0 && stageSize.height > 0) {
            val fit = metrics.grid(stageSize.width.toFloat(), stageSize.height.toFloat())
            // A local of the delegated state read: only a val smart-casts.
            val size = sessionSize
            AgentSession.Grid(
                fit.columns,
                when {
                    (keyboardShown || composing) && size != null && size.columns == fit.columns -> size.rows
                    else -> fit.rows
                },
            )
        } else {
            sessionSize
        }
    }
    val currentReopenGrid by rememberUpdatedState(reopenGrid)

    // Leave the agent while in the background; pick it up again on return.
    // Agents never die with the app; only the attachment comes and goes.
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    var backgrounded by remember { mutableStateOf(false) }
    DisposableEffect(lifecycle, session) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_STOP -> {
                    backgrounded = true
                    // The composer's focus state cannot outlive the stop:
                    // fit() would otherwise keep classifying resizes as
                    // keyboard-only for a keyboard nobody is using.
                    composing = false
                    session.close()
                }
                Lifecycle.Event.ON_START -> {
                    if (backgrounded) {
                        backgrounded = false
                        val current = session.state.value
                        val reopen = current !is AgentSession.State.Closed || current.reopen
                        val grid = currentReopenGrid
                        if (grid != null && reopen) {
                            session.open(grid.columns, grid.rows)
                        }
                    }
                }
                else -> {}
            }
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }

    fun fit(force: Boolean = false) {
        // Until the saved settings land, the geometry is the default's, not
        // the user's; opening now means a resize the moment they apply.
        if (!settingsLoaded) return
        // Nor may a size measured under a bar visibility other than the
        // configured one open the session. The provenance flag records
        // the visibility the bar was rendered under at measurement time:
        // with the bar saved on, the pre-read measurement is tall (no bar
        // renders until the read settles), so the pair disagrees until
        // the bar's own relayout delivers the short geometry — one open,
        // at the configured size. With the bar saved off, the tall
        // pre-read measurement already matches and the session opens as
        // soon as the read lands.
        if (laidOutBarEnabled != commandBarEnabled) return
        if (stageSize.width <= 0 || stageSize.height <= 0) return
        val grid = metrics.grid(stageSize.width.toFloat(), stageSize.height.toFloat())
        val current = session.size.value
        if (current == null) {
            session.open(grid.columns, grid.rows)
            return
        }
        // Focus can precede the keyboard insets, so an unchanged width with
        // the composer active and the height shrinking is a keyboard-only
        // change. A growing height fits rows even while composing: focus
        // without a visible keyboard must never pin the grid short. Rotation
        // and fold changes move the width: fit rows even with the keyboard.
        val keyboardOnly = (keyboardShown || (composing && grid.rows < current.rows)) &&
            grid.columns == current.columns
        if (force || grid.columns != current.columns || (!keyboardOnly && grid.rows != current.rows)) {
            session.resize(grid.columns, if (keyboardOnly) current.rows else grid.rows)
        }
    }

    // settingsLoaded is a key: the gate itself releases fit() when the
    // saved values have applied, including when the stage size never
    // changed (saved state equal to the defaults). The bar pair is keyed
    // the same way: a saved-on entry holds fit() from the read until the
    // bar's relayout records the short geometry, while a saved-off entry
    // agrees at the read itself — its tall pre-read measurement was
    // taken under bar-off.
    LaunchedEffect(session, stageSize, settingsLoaded, laidOutBarEnabled, commandBarEnabled) { fit() }
    LaunchedEffect(session, fontSize) { fit(force = true) }
    // Escape hatch for the provenance gate above: onSizeChanged releases it
    // only when the bar's departure changes the box's height — the only case
    // the gate exists for. If a future bar layout stops affecting the
    // measured size (an overlay-drawn bar, a height absorbed elsewhere), no
    // measurement would ever arrive and the gate would hold the open
    // forever, silently. Two frames after a bar change the layout under the
    // new state has certainly run, and a bar whose exit leaves the size
    // unchanged means the previous geometry is already the right one, so
    // releasing on it opens exactly what the user configured. The normal
    // path still wins the race: onSizeChanged fires during the first of
    // those frames and writes the same value first. Keyed on the rendered
    // visibility, not the raw setting: the read settling the bar into the
    // layout (barShown false→true) is exactly such a relayout.
    LaunchedEffect(barShown) {
        withFrameNanos {}
        withFrameNanos {}
        laidOutBarEnabled = barShown
    }

    fun setFontSize(value: Float) {
        fontSize = value
        // A launch override wins for this run only, matching iOS: it applies
        // in memory and is never written back to settings. The write runs on
        // the application-lived session scope so leaving the stage mid-edit
        // cannot cancel it.
        if (fontSizeOverride == null) {
            sessionScope.launch { settings.putString(FONT_SIZE_KEY, value.toString()) }
        }
    }

    var menuOpen by remember { mutableStateOf(false) }
    val isLive = state == AgentSession.State.Live

    notice?.let { text ->
        AlertDialog(
            onDismissRequest = { session.clearNotice() },
            title = { Text("lapis") },
            text = { Text(text) },
            confirmButton = {
                TextButton(onClick = { session.clearNotice() }) { Text("OK") }
            },
        )
    }

    Scaffold(
        containerColor = LapisColors.background,
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        agent.title,
                        style = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 17.sp),
                    )
                },
                navigationIcon = {
                    IconButton(onClick = onBack, modifier = Modifier.testTag("back")) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    IconButton(onClick = { menuOpen = true }) {
                        Icon(Icons.Filled.MoreVert, contentDescription = "View menu")
                    }
                    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                        DropdownMenuItem(
                            text = { Text("Larger text") },
                            onClick = {
                                menuOpen = false
                                setFontSize(min(fontSize + 1f, MAX_FONT_SIZE))
                            },
                        )
                        DropdownMenuItem(
                            text = { Text("Smaller text") },
                            onClick = {
                                menuOpen = false
                                setFontSize(max(fontSize - 1f, MIN_FONT_SIZE))
                            },
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = LapisColors.background,
                    titleContentColor = Color.White,
                ),
            )
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .imePadding(),
        ) {
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .onSizeChanged {
                        stageSize = it
                        // The size and the bar visibility that produced it
                        // must move together, or fit() would compare a new
                        // size against a stale provenance. barShown, not
                        // commandBarEnabled: this callback runs during the
                        // layout of the composition that installed it, and
                        // that composition's barShown is the visibility the
                        // measurement was actually taken under — the raw
                        // setting can disagree with it for the whole
                        // pre-read window.
                        laidOutBarEnabled = barShown
                    }
                    .pointerInput(Unit) {
                        detectTapGestures { composerFocus.requestFocus() }
                    },
            ) {
                TerminalScreen(
                    frame = frame,
                    history = history,
                    historyEnd = historyEnd,
                    loadingHistory = loadingHistory,
                    fitColumns = reopenGrid?.columns ?: 80,
                    metrics = metrics,
                    loadOlder = loadOlder,
                    onWheel = remember(session) {
                        { notches: Int, column: Int, row: Int ->
                            session.send(Input.wheel(notches, column, row))
                        }
                    },
                )
            }
            Banner(state, agent.title, shared, reopenGrid, onReopen)
            if (barShown) {
                CommandBar(
                    live = isLive,
                    snippets = snippets,
                    snippetsReady = snippetsLoaded,
                    onInput = remember(session) {
                        { input: Input -> session.send(input) }
                    },
                    onSnippets = setSnippets,
                )
            }
            Composer(
                draft = draft,
                onDraft = { draft = it },
                live = isLive,
                focus = composerFocus,
                onFocus = { composing = it },
                onType = {
                    session.send(Input(text = draft))
                    draft = ""
                },
                onSubmit = {
                    session.submit(draft)
                    draft = ""
                },
            )
        }
    }
}

@Composable
private fun Banner(
    state: AgentSession.State,
    title: String,
    shared: Boolean,
    size: AgentSession.Grid?,
    onReopen: (columns: Int, rows: Int) -> Unit,
) {
    when (state) {
        AgentSession.State.Connecting -> Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier
                .fillMaxWidth()
                .background(LapisColors.panel)
                .padding(8.dp),
        ) {
            CircularProgressIndicator(
                modifier = Modifier.size(16.dp),
                strokeWidth = 2.dp,
                color = LapisColors.quiet,
            )
            Text(
                "Opening $title…",
                color = LapisColors.quiet,
                style = TextStyle(fontSize = 13.sp),
            )
        }
        is AgentSession.State.Closed -> Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier
                .fillMaxWidth()
                .background(LapisColors.panel)
                .padding(10.dp),
        ) {
            Text(
                state.reason,
                color = Color.White,
                style = TextStyle(fontSize = 13.sp),
            )
            if (size != null) {
                OutlinedButton(
                    onClick = { onReopen(size.columns, size.rows) },
                    modifier = Modifier.testTag("reopen"),
                ) { Text("Open here again") }
            }
        }
        AgentSession.State.Live -> if (!shared) {
            Text(
                "This agent started before sync, so the Mac shows it as taken over. Restarted agents stay in sync.",
                color = LapisColors.quiet,
                style = TextStyle(fontSize = 12.sp),
                modifier = Modifier
                    .fillMaxWidth()
                    .background(LapisColors.panel)
                    .padding(6.dp),
            )
        }
    }
}

@Composable
private fun Composer(
    draft: String,
    onDraft: (String) -> Unit,
    live: Boolean,
    focus: FocusRequester,
    onFocus: (Boolean) -> Unit,
    onType: () -> Unit,
    onSubmit: () -> Unit,
) {
    Row(
        verticalAlignment = Alignment.Bottom,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        modifier = Modifier
            .fillMaxWidth()
            .background(LapisColors.background)
            .padding(horizontal = 10.dp, vertical = 8.dp),
    ) {
        BasicTextField(
            value = draft,
            onValueChange = onDraft,
            modifier = Modifier
                .weight(1f)
                .testTag("composer")
                .focusRequester(focus)
                .onFocusChanged { onFocus(it.isFocused) }
                .background(LapisColors.panel, RoundedCornerShape(18.dp))
                .border(1.dp, LapisColors.edge, RoundedCornerShape(18.dp))
                .padding(horizontal = 12.dp, vertical = 8.dp),
            textStyle = TextStyle(color = Color.White, fontSize = 16.sp),
            cursorBrush = androidx.compose.ui.graphics.SolidColor(LapisColors.accent),
            decorationBox = { inner ->
                if (draft.isEmpty()) {
                    Text("Message", color = LapisColors.quiet, style = TextStyle(fontSize = 16.sp))
                }
                inner()
            },
            minLines = 1,
            maxLines = 6,
        )
        IconButton(onClick = onType, enabled = draft.isNotEmpty() && live, modifier = Modifier.testTag("type")) {
            Icon(KeyboardGlyph, contentDescription = "Type without Enter")
        }
        IconButton(onClick = onSubmit, enabled = live, modifier = Modifier.testTag("send")) {
            Icon(Icons.AutoMirrored.Filled.Send, contentDescription = "Send")
        }
    }
}

/**
 * A keyboard silhouette (outline, two rows of keys, a space bar): the composer's
 * type-without-Enter affordance, hand-drawn so the app stays on the core icon set.
 */
private val KeyboardGlyph by lazy {
    androidx.compose.ui.graphics.vector.ImageVector.Builder(
        name = "KeyboardGlyph",
        defaultWidth = 24.dp,
        defaultHeight = 24.dp,
        viewportWidth = 24f,
        viewportHeight = 24f,
    ).apply {
        addPath(
            pathData = addPathNodes("M3.5,7 h17 v10 h-17 z"),
            stroke = androidx.compose.ui.graphics.SolidColor(LapisColors.quiet),
            strokeLineWidth = 1.7f,
            strokeLineCap = androidx.compose.ui.graphics.StrokeCap.Round,
            strokeLineJoin = androidx.compose.ui.graphics.StrokeJoin.Round,
        )
        val keys = buildString {
            for (row in 0..1) {
                val y = 9.5f + row * 2.4f
                for (column in 0..4) {
                    val x = 5.8f + column * 2.5f
                    append("M$x,${y} h1.3 v1.3 h-1.3 z ")
                }
            }
            append("M8.6,14.9 h6.8 v1.3 h-6.8 z")
        }
        addPath(
            pathData = addPathNodes(keys),
            fill = androidx.compose.ui.graphics.SolidColor(LapisColors.quiet),
        )
    }.build()
}

/** DataStore key for the terminal font size, matching iOS "terminalFontSize". */
internal const val FONT_SIZE_KEY = "terminalFontSize"

/** DataStore key for the command bar's visibility, matching iOS "commandBarEnabled". */
internal const val COMMAND_BAR_KEY = "commandBarEnabled"

private const val DEFAULT_FONT_SIZE = 12f
internal const val MIN_FONT_SIZE = 8f
internal const val MAX_FONT_SIZE = 20f

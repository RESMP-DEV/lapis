package dev.lapis.remote.terminal

import android.icu.lang.UCharacter
import android.icu.lang.UProperty
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.lapis.remote.gateway.Run
import dev.lapis.remote.gateway.ScreenFrame
import dev.lapis.remote.session.HistoryChunk
import java.text.BreakIterator
import java.util.UUID
import kotlin.math.max

/** `#RRGGBB` only, as the iOS `Color(hex:)`; anything else falls back to the caller's color. */
fun terminalColor(hex: String?): Color? {
    if (hex == null || hex.length != 7 || !hex.startsWith("#")) return null
    val value = hex.substring(1).toLongOrNull(16) ?: return null
    return Color(
        red = ((value shr 16) and 0xFF).toInt() / 255f,
        green = ((value shr 8) and 0xFF).toInt() / 255f,
        blue = (value and 0xFF).toInt() / 255f,
    )
}

/** Symbols that are text by default but also have an emoji form, such as
 *  Claude Code's ⏺ before each message or ⏸, keep the text form the Mac's
 *  terminal draws, one cell wide: Android would otherwise draw them as emoji
 *  tiles two cells wide. A program asking for the emoji (U+FE0F) keeps it.
 *  Port of `textPresentation` in TerminalScreen.swift. */
internal fun textPresentation(string: String): String {
    if (string.all { it.code <= 0x7F }) return string
    val iterator = graphemeIterator.get()
    iterator.setText(string)
    val shown = StringBuilder(string.length)
    var start = iterator.first()
    var end = iterator.next()
    while (end != BreakIterator.DONE) {
        val cluster = string.substring(start, end)
        shown.append(cluster)
        // Single-scalar clusters only: a cluster that already carries a
        // variation selector or joins a sequence has chosen its form.
        if (cluster.length == cluster.codePointCount(0, cluster.length)) {
            val codePoint = cluster.codePointAt(0)
            if (emojiCapableText(codePoint)) shown.appendCodePoint(0xFE0E)
        }
        start = end
        end = iterator.next()
    }
    iterator.setText("")
    return shown.toString()
}

private val graphemeIterator = ThreadLocal.withInitial<BreakIterator> {
    BreakIterator.getCharacterInstance()
}

private fun emojiCapableText(codePoint: Int): Boolean =
    codePoint > 0x7F &&
        UCharacter.getIntPropertyValue(codePoint, UProperty.EMOJI) == 1 &&
        UCharacter.getIntPropertyValue(codePoint, UProperty.EMOJI_PRESENTATION) == 0

/**
 * One terminal row. Backgrounds fill the whole row height and each run sits
 * at its own column, so shaded blocks join without seams between rows.
 * Port of `TerminalRow` in TerminalScreen.swift onto the Compose Canvas.
 */
@Composable
fun TerminalRow(
    runs: List<Run>,
    columns: Int,
    metrics: TerminalMetrics,
    foreground: Color,
    background: Color,
) {
    val density = LocalDensity.current
    val scale = density.density
    val measurer = rememberTextMeasurer(cacheSize = 128)
    val fontSize = with(density) { metrics.fontSize.toSp() }
    Canvas(
        Modifier.size(
            width = with(density) { (columns * metrics.cellWidth).toDp() },
            height = with(density) { metrics.lineHeight.toDp() },
        ),
    ) {
        var next = 0
        for (run in runs) {
            val column = run.column ?: next
            val cells = run.width ?: Wrap.characters(run.text).size
            next = column + cells
            var ink = terminalColor(run.foreground) ?: foreground
            var paper = terminalColor(run.background)
            if (run.flags and Run.CURSOR != 0) {
                val swap = paper ?: background
                paper = ink
                ink = swap
            }
            if (paper != null) {
                val rect = snapped(
                    Rect(
                        column * metrics.cellWidth,
                        0f,
                        (column + cells) * metrics.cellWidth,
                        size.height,
                    ),
                    scale,
                )
                drawRect(paper, topLeft = Offset(rect.left, rect.top), size = rect.size)
            }
            if (run.text.all { it == ' ' }) continue
            if (run.flags and Run.FAINT != 0) ink = ink.copy(alpha = ink.alpha * 0.55f)
            drawRun(run, column, cells, ink, metrics, scale, fontSize, measurer)
        }
    }
}

/** Text is drawn in stretches; block and box characters as cell shapes. */
private fun DrawScope.drawRun(
    run: Run,
    column: Int,
    cells: Int,
    ink: Color,
    metrics: TerminalMetrics,
    scale: Float,
    fontSize: TextUnit,
    measurer: androidx.compose.ui.text.TextMeasurer,
) {
    val characters = Wrap.characters(run.text)
    if (characters.size != cells || characters.none { isCellDrawn(it) }) {
        drawStretch(run.text, run.flags, column, ink, metrics, fontSize, measurer)
        return
    }
    val pending = StringBuilder()
    var pendingColumn = column
    for ((offset, character) in characters.withIndex()) {
        if (isCellDrawn(character)) {
            drawStretch(pending.toString(), run.flags, pendingColumn, ink, metrics, fontSize, measurer)
            pending.setLength(0)
            drawCellGlyph(
                character,
                Rect(
                    (column + offset) * metrics.cellWidth,
                    0f,
                    (column + offset + 1) * metrics.cellWidth,
                    size.height,
                ),
                ink,
                scale,
            )
            pendingColumn = column + offset + 1
        } else {
            pending.append(character)
        }
    }
    drawStretch(pending.toString(), run.flags, pendingColumn, ink, metrics, fontSize, measurer)
}

private fun DrawScope.drawStretch(
    string: String,
    flags: Int,
    column: Int,
    ink: Color,
    metrics: TerminalMetrics,
    fontSize: TextUnit,
    measurer: androidx.compose.ui.text.TextMeasurer,
) {
    val shown = textPresentation(string)
    if (shown.isEmpty() || shown.all { it == ' ' }) return
    val decoration = when {
        flags and Run.UNDERLINE != 0 && flags and Run.STRIKE != 0 ->
            TextDecoration.combine(listOf(TextDecoration.Underline, TextDecoration.LineThrough))
        flags and Run.UNDERLINE != 0 -> TextDecoration.Underline
        flags and Run.STRIKE != 0 -> TextDecoration.LineThrough
        else -> null
    }
    val layout = measurer.measure(
        AnnotatedString(shown),
        TextStyle(
            fontFamily = FontFamily.Monospace,
            fontSize = fontSize,
            fontWeight = if (flags and Run.BOLD != 0) FontWeight.Bold else FontWeight.Normal,
            fontStyle = if (flags and Run.ITALIC != 0) FontStyle.Italic else FontStyle.Normal,
            textDecoration = decoration,
        ),
    )
    drawText(
        layout,
        color = ink,
        topLeft = Offset(column * metrics.cellWidth, (size.height - layout.size.height) / 2f),
    )
}

/**
 * The agent's screen as the Mac's terminal engine drew it: archived pages
 * above the live frame, in a lazily drawn column. Port of `TerminalScreen`
 * in TerminalScreen.swift. Loaded pages are immutable; wrapping is retained
 * while live frames change and rebuilt only when the page set or width
 * changes. New output keeps the bottom in place unless the user scrolled
 * away; coming within a screen of the top loads the previous page.
 */
@Composable
fun TerminalScreen(
    frame: ScreenFrame?,
    history: List<HistoryChunk>,
    historyEnd: Boolean,
    loadingHistory: Boolean,
    fitColumns: Int,
    metrics: TerminalMetrics,
    loadOlder: suspend () -> Unit,
    onWheel: ((notches: Int, column: Int, row: Int) -> Unit)? = null,
) {
    val cache = remember { HistoryCache() }
    cache.prepare(history, fitColumns, frame?.text ?: "")
    val liveRows = frame?.let { Wrap.rows(it.lines, it.columns, fitColumns, "live") } ?: emptyList()
    val background = terminalColor(frame?.background) ?: Color.Black
    val foreground = terminalColor(frame?.foreground) ?: Color.White
    // A full-screen program that takes the wheel shows only its screen: a
    // drag scrolls the program through onWheel, not the archive above it.
    val fullScreen = frame?.wheel == true
    val listState = rememberLazyListState()
    val density = LocalDensity.current

    var viewportWidth by remember { mutableStateOf(0) }
    var viewportHeight by remember { mutableStateOf(0) }
    var followBottom by remember { mutableStateOf(true) }
    var dragging by remember { mutableStateOf(false) }

    // Follow-bottom starts on (the iOS default anchor is the bottom) and only
    // the user's own drag can give it up, at the moment the drag ends; growth
    // or programmatic scrolls never unstick it.
    LaunchedEffect(listState) {
        listState.interactionSource.interactions.collect { interaction ->
            when (interaction) {
                is DragInteraction.Start -> dragging = true
                is DragInteraction.Stop, is DragInteraction.Cancel -> {
                    dragging = false
                    val info = listState.layoutInfo
                    val last = info.visibleItemsInfo.lastOrNull()
                    followBottom = last != null && last.index >= info.totalItemsCount - 1 &&
                        last.offset + last.size <= info.viewportEndOffset + metrics.lineHeight
                }
            }
        }
    }

    // Never while the finger is down: output arriving mid-drag must not yank
    // the anchor out from under it. Re-keyed on `dragging` so a revision that
    // landed during the drag still bottom-aligns once it ends at the bottom.
    // viewportHeight is 0 before the first layout; scrolling then would push
    // the last row past the top for one frame until the real height arrives.
    // Keyed on fullScreen too: leaving a wheel-taking program re-anchors even
    // if revisions ever repeat across the transition. requestScrollToItem
    // takes effect at the next remeasure, before new rows are placed, so the
    // anchor lands without the one-frame offset a post-layout scroll shows.
    LaunchedEffect(frame?.revision, cache.rows.size, liveRows.size, viewportHeight, dragging, fullScreen) {
        if (followBottom && !dragging && viewportHeight > 0 &&
            listState.layoutInfo.totalItemsCount > 0
        ) {
            val last = listState.layoutInfo.totalItemsCount - 1
            // Bottom-align the last row: the offset moves the row's top up
            // from the viewport top, so the negative value here leaves
            // exactly the last row's height visible at the bottom.
            listState.requestScrollToItem(last, metrics.lineHeight.toInt() - viewportHeight)
        }
    }

    // A fling keeps scrolling after the finger lifts, so the drag-end
    // evaluation above can miss the list settling at the bottom; re-anchor
    // whenever any scroll (drag, fling, programmatic) comes to rest there.
    // snapshotFlow emits its current value the moment collection starts, and
    // a list that has never been laid out (first composition, or the whole
    // time a wheel-taking program keeps the archive uncomposed) reports no
    // items; that emission must not touch the anchor or the screen would
    // open and return at the top.
    LaunchedEffect(listState) {
        snapshotFlow { listState.isScrollInProgress }
            .collect { inProgress ->
                if (!inProgress && listState.layoutInfo.totalItemsCount > 0) {
                    val info = listState.layoutInfo
                    val last = info.visibleItemsInfo.lastOrNull()
                    followBottom = last != null && last.index >= info.totalItemsCount - 1 &&
                        last.offset + last.size <= info.viewportEndOffset + metrics.lineHeight
                }
            }
    }

    // Within a screen of the top, the page before loads; each page moves the
    // top away again, so history loads as it is read. Hidden while a
    // wheel-taking program shows: loading the archive changes nothing visible.
    val nearTop by remember(listState) {
        derivedStateOf {
            val info = listState.layoutInfo
            val first = info.visibleItemsInfo.firstOrNull() ?: return@derivedStateOf false
            first.index == 0 && first.offset < with(density) {
                max(viewportHeight, 200.dp.roundToPx())
            }
        }
    }
    LaunchedEffect(nearTop) {
        if (nearTop && !fullScreen) loadOlder()
    }
    // Output may have archived rows while the top is in view.
    LaunchedEffect(frame?.revision) {
        if (nearTop && !fullScreen && history.isEmpty()) loadOlder()
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(background)
            .onSizeChanged {
                viewportWidth = it.width
                viewportHeight = it.height
            }
            .semantics {
                testTag = "terminal"
                // The visible text is the element's description, as the
                // iOS accessibilityValue: TalkBack reads the screen and
                // uiautomator (text and content-desc only) exposes it to
                // device automation. A wheel-taking program hides the
                // archive, so only its screen is read then. The
                // concatenated text is scroll-position-independent, so the
                // description leads with the follow state — the one
                // scroll-dependent fact — as a short spoken sentence:
                // TalkBack announces whether the reader is at the live
                // bottom, and the device harness gets a real anchor for
                // "the drag surrendered follow" and "scrolling returned
                // to the live bottom".
                val screenText = (
                    if (fullScreen) frame?.text.orEmpty() else cache.accessibleText
                    )
                // The state stays in contentDescription (not a separate
                // stateDescription): uiautomator's XML dump — the harness's
                // only window into the accessibility tree — exposes text and
                // content-desc only, and carrying the sentence in both
                // properties would make TalkBack announce it twice per
                // focus. Revisit only with a measured dump showing
                // state-desc visible, moving the anchor with it.
                contentDescription = (
                    if (fullScreen) ""
                    else if (followBottom) "Following live output. "
                    else "Reading earlier output. "
                    ) + screenText.ifBlank { "Agent screen" }
            }
            .pointerInput(fullScreen, metrics.cellWidth, metrics.lineHeight, fitColumns, viewportWidth, onWheel) {
                if (!fullScreen || onWheel == null) return@pointerInput
                // The iOS turnWheel: a notch for every two rows dragged, sent
                // as deltas from the cell where the gesture started.
                var wheelSent = 0
                var totalDy = 0f
                var start = Offset.Zero
                detectVerticalDragGestures(
                    onDragStart = { offset -> start = offset },
                    onDragEnd = { wheelSent = 0; totalDy = 0f },
                    onDragCancel = { wheelSent = 0; totalDy = 0f },
                ) { change, dy ->
                    change.consume()
                    totalDy += dy
                    val notches = (totalDy / (metrics.lineHeight * 2)).toInt()
                    if (notches == wheelSent) return@detectVerticalDragGestures
                    // Both branches center their rows, and the Compose stage
                    // carries no side padding, so the drawing's left edge is
                    // exactly `left`; the iOS port's 4pt pad does not exist
                    // here and must not shift the reported column.
                    val content = fitColumns * metrics.cellWidth
                    val left = (viewportWidth - content) / 2f
                    val column = ((start.x - left) / metrics.cellWidth).toInt()
                    val row = (start.y / metrics.lineHeight).toInt()
                    val frameNow = frame ?: return@detectVerticalDragGestures
                    onWheel(
                        notches - wheelSent,
                        column.coerceIn(0, (frameNow.columns - 1).coerceAtLeast(0)),
                        row.coerceIn(0, (frameNow.rows - 1).coerceAtLeast(0)),
                    )
                    wheelSent = notches
                }
            },
    ) {
        // A wheel-taking program shows one fixed screen that fits the stage;
        // a plain column keeps the phone's scroll machinery (scrollable,
        // overscroll stretch) from consuming the drag the program wants.
        // Centered like the LazyColumn branch so the screen does not shift
        // when the program takes the wheel, and the wheel math's centered
        // origin stays the drawing's origin.
        if (fullScreen) {
            Column(
                Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                for (row in liveRows) {
                    TerminalRow(row.runs, row.columns, metrics, foreground, background)
                }
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                if (frame != null) {
                    item(key = "edge") { HistoryEdge(historyEnd, loadingHistory) }
                }
                items(cache.rows, key = { it.id }) { row ->
                    TerminalRow(row.runs, row.columns, metrics, foreground, background)
                }
                items(liveRows, key = { it.id }) { row ->
                    TerminalRow(row.runs, row.columns, metrics, foreground, background)
                }
            }
        }
    }
}

/** Shown above the oldest loaded row: "Start of history" while more can load. */
@Composable
private fun HistoryEdge(historyEnd: Boolean, loadingHistory: Boolean) {
    if (historyEnd) {
        Text(
            "Start of history",
            color = Color(0.55f, 0.55f, 0.55f),
            style = TextStyle(fontSize = 11.sp, fontFamily = FontFamily.Monospace),
            textAlign = TextAlign.Center,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 4.dp)
                .height(24.dp),
        )
    } else {
        // Only while a page loads; an empty archive shows nothing.
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 4.dp)
                .height(24.dp),
        ) {
            if (loadingHistory) {
                CircularProgressIndicator(
                    modifier = Modifier.size(12.dp),
                    strokeWidth = 1.5.dp,
                    color = Color(0.55f, 0.55f, 0.55f),
                )
                Spacer(Modifier.width(6.dp))
                Text(
                    "Earlier output",
                    color = Color(0.55f, 0.55f, 0.55f),
                    style = TextStyle(fontSize = 11.sp, fontFamily = FontFamily.Monospace),
                )
            }
        }
    }
}

/** Retains wrapping and accessibility text while live frames change. */
private class HistoryCache {
    private var pages: List<UUID> = emptyList()
    private var width = 0
    var rows: List<Wrap.Row> = emptyList()
        private set

    /** Snapshot state: the semantics block reads it, so a change re-applies
     *  semantics and reaches the accessibility tree (TalkBack, uiautomator)
     *  even when no row is added or scrolled — a same-line edit must be
     *  announced too. */
    var accessibleText by mutableStateOf("")
        private set
    private var text = ""
    private var lastLiveText: String? = null

    fun prepare(history: List<HistoryChunk>, width: Int, liveText: String) {
        val ids = history.map { it.cacheId }
        if (ids != pages || width != this.width) {
            pages = ids
            this.width = width
            rows = history.flatMap { chunk ->
                // The cacheId, not the page number: pages can repeat across
                // attachments and generations, and duplicate row keys crash
                // the LazyColumn.
                Wrap.rows(chunk.lines, chunk.columns, width, "h${chunk.cacheId}")
            }
            text = history.flatMap { it.lines }
                .joinToString("\n") { line -> line.joinToString("") { it.text } }
            lastLiveText = null
        }
        if (lastLiveText != liveText) {
            accessibleText = if (text.isEmpty()) liveText else "$text\n$liveText"
            lastLiveText = liveText
        }
    }
}

package dev.lapis.remote.ui.commandbar

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.lapis.remote.gateway.Input
import dev.lapis.remote.ui.LapisColors

/**
 * The permanent command bar below the stage: the fixed key row and the
 * user's snippet row. Compact widths scroll each row horizontally;
 * expanded widths (840dp and wider) wrap the same chips into fuller
 * rows. Chips are inert while the attachment is not live; long-pressing
 * a snippet (or tapping +) opens the editor, which works offline.
 */
@OptIn(ExperimentalFoundationApi::class, ExperimentalLayoutApi::class)
@Composable
fun CommandBar(
    live: Boolean,
    snippets: List<String>,
    onInput: (Input) -> Unit,
    onSnippets: (List<String>) -> Unit,
    modifier: Modifier = Modifier,
) {
    // Saveable so a fold/unfold (activity recreation) cannot discard an
    // open editor with uncommitted rows.
    var editing by rememberSaveable { mutableStateOf(false) }
    Column(
        modifier
            .fillMaxWidth()
            .background(LapisColors.panel)
            .testTag("command-bar"),
    ) {
        // screenWidthDp follows the fold state: unfolding changes the
        // Configuration and recomposes this bar with fuller rows.
        val wide = LocalConfiguration.current.screenWidthDp >= 840
        KeyRow(live = live, onInput = onInput, wide = wide)
        SnippetRow(
            live = live,
            snippets = snippets,
            onInput = onInput,
            onEdit = { editing = true },
            wide = wide,
        )
    }
    if (editing) {
        SnippetEditorDialog(
            snippets = snippets,
            onDone = { next ->
                editing = false
                onSnippets(next)
            },
            onDismiss = { editing = false },
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun KeyRow(live: Boolean, onInput: (Input) -> Unit, wide: Boolean) {
    if (wide) {
        FlowRow(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 3.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            CommandBarKeys.fixed.forEach { key -> KeyChip(key, live, onInput) }
        }
    } else {
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 8.dp, vertical = 3.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            CommandBarKeys.fixed.forEach { key -> KeyChip(key, live, onInput) }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class, ExperimentalLayoutApi::class)
@Composable
private fun SnippetRow(
    live: Boolean,
    snippets: List<String>,
    onInput: (Input) -> Unit,
    onEdit: () -> Unit,
    wide: Boolean,
) {
    val content: @Composable () -> Unit = {
        snippets.forEachIndexed { index, snippet ->
            SnippetChip(
                index = index,
                snippet = snippet,
                enabled = live,
                onRun = { onInput(CommandBarKeys.snippet(snippet)) },
                onEdit = onEdit,
            )
        }
        AddChip(onEdit)
    }
    if (wide) {
        FlowRow(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 3.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) { content() }
    } else {
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 8.dp, vertical = 3.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) { content() }
    }
}

@Composable
private fun KeyChip(key: CommandKey, enabled: Boolean, onInput: (Input) -> Unit) {
    Text(
        key.label,
        color = if (enabled) Color.White else LapisColors.quiet,
        style = TextStyle(fontSize = 13.sp, fontFamily = FontFamily.Monospace),
        modifier = Modifier
            .testTag(key.testTag)
            .chipLook(enabled)
            .clickable(enabled = enabled) { onInput(key.payload) },
    )
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun SnippetChip(
    index: Int,
    snippet: String,
    enabled: Boolean,
    onRun: () -> Unit,
    onEdit: () -> Unit,
) {
    Text(
        snippet,
        color = if (enabled) LapisColors.accent else LapisColors.quiet,
        style = TextStyle(fontSize = 13.sp, fontFamily = FontFamily.Monospace),
        // One line: a long snippet must not grow the row or wrap across
        // the wide layout; the full text is what the editor shows.
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier
            .testTag("snippet-$index")
            .chipLook(enabled)
            // Long-press opens the editor even while offline: editing the
            // list is not a terminal operation.
            .combinedClickable(
                onClick = { if (enabled) onRun() },
                onLongClick = onEdit,
            ),
    )
}

@Composable
private fun AddChip(onEdit: () -> Unit) {
    Text(
        "+",
        color = Color.White,
        style = TextStyle(fontSize = 15.sp, fontFamily = FontFamily.Monospace),
        modifier = Modifier
            .testTag("snippets-add")
            .chipLook(enabled = true)
            .clickable { onEdit() },
    )
}

private fun Modifier.chipLook(enabled: Boolean): Modifier = this
    .clip(RoundedCornerShape(8.dp))
    .background(
        when (enabled) {
            true -> LapisColors.panelDeep
            false -> LapisColors.background
        },
    )
    .border(1.dp, LapisColors.edge, RoundedCornerShape(8.dp))
    .padding(horizontal = 10.dp, vertical = 5.dp)

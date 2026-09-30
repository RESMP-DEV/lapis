package dev.lapis.remote.ui.commandbar

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.lapis.remote.ui.LapisColors

/**
 * Edits the snippet list: rows are editable in place, arrows reorder,
 * the close button removes, the field at the bottom adds. Done commits
 * the trimmed, non-empty list; Cancel discards. Pure phone state —
 * nothing here talks to the gateway.
 */
@Composable
fun SnippetEditorDialog(
    snippets: List<String>,
    onDone: (List<String>) -> Unit,
    onDismiss: () -> Unit,
) {
    var rows by remember(snippets) { mutableStateOf(snippets.toList()) }
    var draft by remember { mutableStateOf("") }

    fun move(index: Int, delta: Int) {
        rows = rows.toMutableList().also { it.add(index + delta, it.removeAt(index)) }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Command snippets") },
        text = {
            Column(
                Modifier
                    // AlertDialog opens its own window, so the root-level
                    // testTagsAsResourceId in MainActivity does not reach
                    // this subtree; without this the harness cannot address
                    // any node inside the dialog by resource id.
                    .semantics { testTagsAsResourceId = true }
                    .testTag("snippet-editor")
                    .verticalScroll(rememberScrollState()),
            ) {
                rows.forEachIndexed { index, text ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(2.dp),
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        OutlinedTextField(
                            value = text,
                            onValueChange = { value ->
                                // Reject rather than truncate: typing simply
                                // stops at the store's per-snippet bound.
                                if (value.length <= SnippetStore.MAX_LENGTH) {
                                    rows = rows.toMutableList().also { it[index] = value }
                                }
                            },
                            modifier = Modifier
                                .weight(1f)
                                .testTag("snippet-edit-$index"),
                            singleLine = true,
                            textStyle = snippetTextStyle,
                        )
                        IconButton(
                            onClick = { move(index, -1) },
                            enabled = index > 0,
                            modifier = Modifier.testTag("snippet-$index-up"),
                        ) {
                            Icon(Icons.Filled.KeyboardArrowUp, contentDescription = "Move up")
                        }
                        IconButton(
                            onClick = { move(index, +1) },
                            enabled = index < rows.lastIndex,
                            modifier = Modifier.testTag("snippet-$index-down"),
                        ) {
                            Icon(Icons.Filled.KeyboardArrowDown, contentDescription = "Move down")
                        }
                        IconButton(
                            onClick = {
                                rows = rows.filterIndexed { position, _ -> position != index }
                            },
                            modifier = Modifier.testTag("snippet-$index-delete"),
                        ) {
                            Icon(Icons.Filled.Close, contentDescription = "Delete")
                        }
                    }
                }
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 8.dp),
                ) {
                    OutlinedTextField(
                        value = draft,
                        onValueChange = { value ->
                            if (value.length <= SnippetStore.MAX_LENGTH) {
                                draft = value
                            }
                        },
                        modifier = Modifier
                            .weight(1f)
                            .testTag("snippet-new"),
                        singleLine = true,
                        placeholder = { Text("New snippet") },
                        textStyle = snippetTextStyle,
                    )
                    TextButton(
                        onClick = {
                            rows = rows + draft.trim()
                            draft = ""
                        },
                        // The count bound surfaces here instead of as a
                        // silent drop when the list is saved.
                        enabled = draft.isNotBlank() && rows.size < SnippetStore.MAX_SNIPPETS,
                        modifier = Modifier.testTag("snippet-add"),
                    ) { Text("Add") }
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    onDone(rows.map { it.trim() }.filter { it.isNotEmpty() })
                },
                modifier = Modifier
                    .semantics { testTagsAsResourceId = true }
                    .testTag("snippet-done"),
            ) { Text("Done") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

private val snippetTextStyle = TextStyle(
    color = Color.White,
    fontSize = 14.sp,
    fontFamily = FontFamily.Monospace,
)

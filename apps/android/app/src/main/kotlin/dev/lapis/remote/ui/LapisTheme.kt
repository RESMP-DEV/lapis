package dev.lapis.remote.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/**
 * Command-room palette ported value-for-value from `Theme` in
 * apps/ios/Lapis/LapisApp.swift: dark opaque surfaces, teal-free quiet grays,
 * a luminous selection accent. Built from the same sRGB floats rather than
 * rounded hex so both platforms render identically.
 */
object LapisColors {
    val accent = Color(0.36f, 0.52f, 1.0f)
    val background = Color(0.0f, 0.0f, 0.0f)
    val panel = Color(0.055f, 0.067f, 0.094f)
    val panelDeep = Color(0.027f, 0.031f, 0.047f)
    val edge = Color(0.18f, 0.18f, 0.18f)
    val quiet = Color(0.55f, 0.55f, 0.55f)
    val live = Color(0.30f, 0.85f, 0.55f)

    /** Each harness's mark and card edge colour. */
    fun harness(id: String): Color = when (id) {
        "claude" -> Color(0.85f, 0.47f, 0.34f)
        "kimi" -> Color(0.40f, 0.60f, 1.0f)
        "omp" -> Color(0.55f, 0.85f, 0.65f)
        "agy" -> Color(0.30f, 0.55f, 1.0f)
        "codex", "grok", "opencode" -> Color(0.9f, 0.9f, 0.9f)
        else -> accent
    }
}

@Composable
fun LapisTheme(content: @Composable () -> Unit) {
    // The command room is dark regardless of the system setting, as the iOS
    // app pins .preferredColorScheme(.dark).
    isSystemInDarkTheme()
    val scheme = darkColorScheme(
        primary = LapisColors.accent,
        background = LapisColors.background,
        surface = LapisColors.panel,
        surfaceContainerLowest = LapisColors.panelDeep,
        surfaceContainer = LapisColors.panel,
        outline = LapisColors.edge,
        onBackground = Color.White,
        onSurface = Color.White,
    )
    MaterialTheme(colorScheme = scheme, content = content)
}

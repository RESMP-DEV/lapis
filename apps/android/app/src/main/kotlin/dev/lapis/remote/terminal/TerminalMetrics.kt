package dev.lapis.remote.terminal

import kotlin.math.ceil
import kotlin.math.max

/**
 * Monospace cell metrics for one font size, in pixels. Port of
 * `TerminalMetrics` in TerminalScreen.swift: the cell is the width of "M",
 * the row is the font's line height.
 */
class TerminalMetrics(
    val fontSize: Float,
    val cellWidth: Float,
    val lineHeight: Float,
) {
    /** The terminal grid that fits a pixel size; both floors are the iOS minimums. */
    fun grid(width: Float, height: Float): Grid = Grid(
        columns = max(20, ((width - 8f) / cellWidth).toInt()),
        rows = max(6, (height / lineHeight).toInt()),
    )

    data class Grid(val columns: Int, val rows: Int)

    companion object {
        /** Measures the platform monospace face, as the iOS port measures the system one. */
        fun system(fontSize: Float): TerminalMetrics {
            val paint = android.graphics.Paint().apply {
                typeface = android.graphics.Typeface.MONOSPACE
                textSize = fontSize
            }
            return TerminalMetrics(
                fontSize = fontSize,
                cellWidth = paint.measureText("M"),
                lineHeight = ceil(paint.fontMetrics.descent - paint.fontMetrics.ascent),
            )
        }
    }
}

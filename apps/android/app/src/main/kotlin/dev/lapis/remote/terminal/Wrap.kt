package dev.lapis.remote.terminal

import dev.lapis.remote.gateway.Run
import java.text.BreakIterator

/**
 * Reflow for rows wider than the phone. Port of `TerminalScreen.wrap` and
 * `rows`: trailing blank cells drop first, runs keep their styles, and a run
 * whose characters do not match its cell count (wide characters) stays whole
 * on the row it starts in.
 */
object Wrap {
    /** One drawn row after reflow, with a stable identity across frames. */
    data class Row(val id: String, val runs: List<Run>, val columns: Int)

    /** Grapheme clusters, matching Swift's `String` elements. */
    fun characters(text: String): List<String> {
        val boundaries = BreakIterator.getCharacterInstance()
        boundaries.setText(text)
        val parts = ArrayList<String>()
        var start = boundaries.first()
        var end = boundaries.next()
        while (end != BreakIterator.DONE) {
            parts.add(text.substring(start, end))
            start = end
            end = boundaries.next()
        }
        return parts
    }

    fun rows(lines: List<List<Run>>, columns: Int, width: Int, prefix: String): List<Row> =
        lines.flatMapIndexed { index, line ->
            wrap(line, columns, width).mapIndexed { part, runs ->
                Row(id = "$prefix-$index.$part", runs = runs, columns = minOf(columns, width))
            }
        }

    fun wrap(line: List<Run>, columns: Int, limit: Int): List<List<Run>> {
        if (columns <= limit || limit <= 0) return listOf(line)
        val runs = line.toMutableList()
        while (runs.isNotEmpty()) {
            val last = runs.last()
            if (last.background != null || last.flags and Run.CURSOR != 0 ||
                last.text.any { it != ' ' }
            ) {
                break
            }
            runs.removeAt(runs.size - 1)
        }
        val rows = ArrayList<MutableList<Run>>()
        rows.add(mutableListOf())
        var next = 0
        for (run in runs) {
            var column = run.column ?: next
            val cells = run.width ?: characters(run.text).size
            next = column + cells
            val characters = characters(run.text)
            // Wide characters: keep the run whole on the row it starts in.
            if (characters.size != cells) {
                place(run, run.text, column, cells, limit, rows)
                continue
            }
            var start = 0
            while (start < characters.size) {
                val room = limit - column % limit
                val take = minOf(room, characters.size - start)
                val piece = characters.subList(start, start + take).joinToString("")
                place(run, piece, column, take, limit, rows)
                start += take
                column += room
            }
        }
        return rows.ifEmpty { listOf(emptyList()) }
    }

    private fun place(
        run: Run,
        text: String,
        column: Int,
        width: Int,
        limit: Int,
        rows: MutableList<MutableList<Run>>,
    ) {
        val row = column / limit
        while (rows.size <= row) rows.add(mutableListOf())
        rows[row].add(
            Run(
                text = text,
                foreground = run.foreground,
                background = run.background,
                flags = run.flags,
                column = column % limit,
                width = width,
            ),
        )
    }
}

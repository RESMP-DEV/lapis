package dev.lapis.remote.terminal

import dev.lapis.remote.gateway.Run
import kotlin.test.Test
import kotlin.test.assertEquals

/**
 * Reflow parity with TerminalScreen.swift: rows that fit pass through, wide
 * rows split at the limit with column resets, trailing blanks drop, and runs
 * whose characters do not match their cell count stay whole.
 */
class WrapTest {
    private fun run(text: String, column: Int? = null, width: Int? = null, background: String? = null) =
        Run(text = text, foreground = null, background = background, flags = 0, column = column, width = width)

    @Test
    fun graphemeClustersMatchSwiftStringElements() {
        assertEquals(listOf("a", "b"), Wrap.characters("ab"))
        assertEquals(listOf("a", "👍", "b"), Wrap.characters("a👍b"))
        // An accent is part of the cluster it follows, as Swift's Character.
        assertEquals(listOf("é"), Wrap.characters("é"))
        assertEquals(emptyList(), Wrap.characters(""))
    }

    @Test
    fun aRowThatFitsPassesThroughUnchanged() {
        val line = listOf(run("hello", column = 0, width = 5))
        assertEquals(listOf(line), Wrap.wrap(line, columns = 40, limit = 80))
    }

    @Test
    fun aZeroLimitNeverReflows() {
        val line = listOf(run("hello", column = 0, width = 5))
        assertEquals(listOf(line), Wrap.wrap(line, columns = 100, limit = 0))
    }

    @Test
    fun aWideRunSplitsAtTheLimitWithColumnsReset() {
        val text = "0".repeat(80)
        val line = listOf(run(text, column = 0, width = 80))
        val rows = Wrap.wrap(line, columns = 80, limit = 40)

        assertEquals(2, rows.size)
        assertEquals(0, rows[0].single().column)
        assertEquals(40, rows[0].single().width)
        assertEquals(0, rows[1].single().column)
        assertEquals(40, rows[1].single().width)
        assertEquals(80, rows.flatMap { it }.sumOf { it.width ?: 0 })
        assertEquals(text, rows.flatMap { it }.joinToString("") { it.text })
    }

    @Test
    fun aRunStartingMidRowSplitsWhereTheRowEnds() {
        val line = listOf(
            run("head:", column = 0, width = 5),
            run("x".repeat(80), column = 5, width = 80),
        )
        val rows = Wrap.wrap(line, columns = 85, limit = 40)

        assertEquals(3, rows.size)
        // Row 0 holds the head and the 35 cells left beside it.
        assertEquals(2, rows[0].size)
        assertEquals(0, rows[0][0].column)
        assertEquals(5, rows[0][1].column)
        assertEquals(35, rows[0][1].width)
        // The rest continues at each new row's first column.
        assertEquals(0, rows[1].single().column)
        assertEquals(40, rows[1].single().width)
        assertEquals(0, rows[2].single().column)
        assertEquals(5, rows[2].single().width)
    }

    @Test
    fun trailingBlankCellsDropBeforeReflow() {
        val line = listOf(
            run("abc", column = 0, width = 3),
            run("   ", column = 3, width = 3),
        )
        val rows = Wrap.wrap(line, columns = 50, limit = 40)

        assertEquals(1, rows.size)
        assertEquals(run("abc", column = 0, width = 3), rows[0].single())
    }

    @Test
    fun trailingBlanksStayWhenTheyPaintBackground() {
        val line = listOf(
            run("abc", column = 0, width = 3),
            run("   ", column = 3, width = 3, background = "#202020"),
        )
        val rows = Wrap.wrap(line, columns = 50, limit = 40)

        assertEquals(1, rows.size)
        assertEquals(2, rows[0].size)
        assertEquals("#202020", rows[0][1].background)
    }

    @Test
    fun aRunWiderThanItsCharactersStaysWhole() {
        // Two graphemes across four cells (wide characters): one piece.
        val line = listOf(run("你好", column = 0, width = 4))
        val rows = Wrap.wrap(line, columns = 4, limit = 3)

        assertEquals(1, rows.size)
        assertEquals("你好", rows[0].single().text)
        assertEquals(0, rows[0].single().column)
        assertEquals(4, rows[0].single().width)
    }

    @Test
    fun rowsBuildStableIdentitiesAndCarryTheDrawnWidth() {
        val lines = listOf(
            listOf(run("0".repeat(80), column = 0, width = 80)),
            listOf(run("tail", column = 0, width = 4)),
        )
        val rows = Wrap.rows(lines, columns = 80, width = 40, prefix = "h7")

        assertEquals(listOf("h7-0.0", "h7-0.1", "h7-1.0"), rows.map { it.id })
        assertEquals(3, rows.size)
        rows.forEach { assertEquals(40, it.columns) }
    }
}

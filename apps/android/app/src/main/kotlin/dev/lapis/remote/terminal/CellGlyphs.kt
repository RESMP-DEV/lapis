package dev.lapis.remote.terminal

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import kotlin.math.atan2
import kotlin.math.max
import kotlin.math.min
import kotlin.math.round

/**
 * Block elements and box drawing drawn as shapes filling their cell, as
 * terminals do, so they join across rows and neighbouring cells. Font glyphs
 * for them are shorter than a row and leave lines through logos and borders.
 * Port of `CellGlyphs` in CellGlyphs.swift onto the Compose Canvas.
 */

/** True when [character] (one grapheme) is drawn as a cell shape. */
fun isCellDrawn(character: String): Boolean {
    if (character.isEmpty()) return false
    val value = character.codePointAt(0)
    if (Character.charCount(value) != character.length) return false
    return value in 0x2580..0x259F || boxArms.containsKey(value) || arcs.containsKey(value)
}

fun DrawScope.drawCellGlyph(character: String, cell: Rect, color: Color, scale: Float) {
    if (character.isEmpty()) return
    val value = character.codePointAt(0)
    when {
        value in 0x2580..0x259F -> drawBlock(value, cell, color, scale)
        boxArms.containsKey(value) -> drawBox(boxArms.getValue(value), cell, color, scale)
        arcs.containsKey(value) -> drawArc(arcs.getValue(value), cell, color, scale)
    }
}

/** Block elements (U+2580–U+259F). */
private fun DrawScope.drawBlock(value: Int, cell: Rect, color: Color, scale: Float) {
    val w = cell.width
    val h = cell.height

    fun fill(x: Float, y: Float, width: Float, height: Float, alpha: Float = 1f) {
        val rect = snapped(
            Rect(cell.left + x, cell.top + y, cell.left + x + width, cell.top + y + height),
            scale,
        )
        drawRect(color.withAlpha(alpha), topLeft = Offset(rect.left, rect.top), size = rect.size)
    }

    when (value) {
        0x2580 -> fill(0f, 0f, w, h / 2)
        in 0x2581..0x2588 -> {
            val part = h * (value - 0x2580) / 8f
            fill(0f, h - part, w, part)
        }
        in 0x2589..0x258F -> fill(0f, 0f, w * (0x2590 - value) / 8f, h)
        0x2590 -> fill(w / 2, 0f, w / 2, h)
        0x2591 -> fill(0f, 0f, w, h, 0.25f)
        0x2592 -> fill(0f, 0f, w, h, 0.5f)
        0x2593 -> fill(0f, 0f, w, h, 0.75f)
        0x2594 -> fill(0f, 0f, w, h / 8)
        0x2595 -> fill(w * 7 / 8, 0f, w / 8, h)
        else -> {
            // Quadrants: upper left, upper right, lower left, lower right.
            val quadrants = mapOf(
                0x2596 to listOf(false, false, true, false),
                0x2597 to listOf(false, false, false, true),
                0x2598 to listOf(true, false, false, false),
                0x2599 to listOf(true, false, true, true),
                0x259A to listOf(true, false, false, true),
                0x259B to listOf(true, true, true, false),
                0x259C to listOf(true, true, false, true),
                0x259D to listOf(false, true, false, false),
                0x259E to listOf(false, true, true, false),
                0x259F to listOf(false, true, true, true),
            )
            val (ul, ur, ll, lr) = quadrants[value] ?: return
            if (ul) fill(0f, 0f, w / 2, h / 2)
            if (ur) fill(w / 2, 0f, w / 2, h / 2)
            if (ll) fill(0f, h / 2, w / 2, h / 2)
            if (lr) fill(w / 2, h / 2, w / 2, h / 2)
        }
    }
}

/** Arms up, right, down, left: 0 none, 1 light, 2 heavy, 3 double. */
private class Arms(val up: Int, val right: Int, val down: Int, val left: Int)

private val boxArms: Map<Int, Arms> = buildMap {
    fun entry(code: Int, up: Int, right: Int, down: Int, left: Int) {
        put(code, Arms(up, right, down, left))
    }
    entry(0x2500, 0, 1, 0, 1); entry(0x2501, 0, 2, 0, 2); entry(0x2502, 1, 0, 1, 0); entry(0x2503, 2, 0, 2, 0)
    entry(0x250C, 0, 1, 1, 0); entry(0x250F, 0, 2, 2, 0); entry(0x2510, 0, 0, 1, 1); entry(0x2513, 0, 0, 2, 2)
    entry(0x2514, 1, 1, 0, 0); entry(0x2517, 2, 2, 0, 0); entry(0x2518, 1, 0, 0, 1); entry(0x251B, 2, 0, 0, 2)
    entry(0x251C, 1, 1, 1, 0); entry(0x2523, 2, 2, 2, 0); entry(0x2524, 1, 0, 1, 1); entry(0x252B, 2, 0, 2, 2)
    entry(0x252C, 0, 1, 1, 1); entry(0x2533, 0, 2, 2, 2); entry(0x2534, 1, 1, 0, 1); entry(0x253B, 2, 2, 0, 2)
    entry(0x253C, 1, 1, 1, 1); entry(0x254B, 2, 2, 2, 2)
    entry(0x2550, 0, 3, 0, 3); entry(0x2551, 3, 0, 3, 0); entry(0x2554, 0, 3, 3, 0); entry(0x2557, 0, 0, 3, 3)
    entry(0x255A, 3, 3, 0, 0); entry(0x255D, 3, 0, 0, 3); entry(0x2560, 3, 3, 3, 0); entry(0x2563, 3, 0, 3, 3)
    entry(0x2566, 0, 3, 3, 3); entry(0x2569, 3, 3, 0, 3); entry(0x256C, 3, 3, 3, 3)
    entry(0x2574, 0, 0, 0, 1); entry(0x2575, 1, 0, 0, 0); entry(0x2576, 0, 1, 0, 0); entry(0x2577, 0, 0, 1, 0)
    entry(0x2578, 0, 0, 0, 2); entry(0x2579, 2, 0, 0, 0); entry(0x257A, 0, 2, 0, 0); entry(0x257B, 0, 0, 2, 0)
    // Dashed lines are drawn solid.
    entry(0x2504, 0, 1, 0, 1); entry(0x2505, 0, 2, 0, 2); entry(0x2506, 1, 0, 1, 0); entry(0x2507, 2, 0, 2, 0)
    entry(0x2508, 0, 1, 0, 1); entry(0x2509, 0, 2, 0, 2); entry(0x250A, 1, 0, 1, 0); entry(0x250B, 2, 0, 2, 0)
    entry(0x254C, 0, 1, 0, 1); entry(0x254D, 0, 2, 0, 2); entry(0x254E, 1, 0, 1, 0); entry(0x254F, 2, 0, 2, 0)
}

/** Box drawing (U+2500–U+257F): each arm runs from the far edge to past the centre, so joints close. */
private fun DrawScope.drawBox(arms: Arms, cell: Rect, color: Color, scale: Float) {
    val light = max(1f / scale, round(cell.width * 0.12f * scale) / scale)
    val cx = cell.center.x
    val cy = cell.center.y

    fun thickness(weight: Int): Float = if (weight == 2) light * 2 else light

    fun fill(rect: Rect) {
        val snappedRect = snapped(rect, scale)
        drawRect(color, topLeft = Offset(snappedRect.left, snappedRect.top), size = snappedRect.size)
    }

    fun horizontal(x0: Float, x1: Float, weight: Int) {
        if (weight == 0) return
        val t = thickness(weight)
        val offsets = if (weight == 3) floatArrayOf(-t * 1.5f, t * 0.5f) else floatArrayOf(-t / 2)
        for (offset in offsets) {
            fill(Rect(min(x0, x1), cy + offset, max(x0, x1), cy + offset + t))
        }
    }

    fun vertical(y0: Float, y1: Float, weight: Int) {
        if (weight == 0) return
        val t = thickness(weight)
        val offsets = if (weight == 3) floatArrayOf(-t * 1.5f, t * 0.5f) else floatArrayOf(-t / 2)
        for (offset in offsets) {
            fill(Rect(cx + offset, min(y0, y1), cx + offset + t, max(y0, y1)))
        }
    }

    val reach = light * 2
    horizontal(cell.left, cx + if (arms.left > 0) reach else 0f, arms.left)
    horizontal(cx - if (arms.right > 0) reach else 0f, cell.right, arms.right)
    vertical(cell.top, cy + if (arms.up > 0) reach else 0f, arms.up)
    vertical(cy - if (arms.down > 0) reach else 0f, cell.bottom, arms.down)
}

/** Rounded corners: which two edges the arc joins. */
private enum class Arc { RIGHT_DOWN, LEFT_DOWN, LEFT_UP, RIGHT_UP }

private val arcs = mapOf(
    0x256D to Arc.RIGHT_DOWN,
    0x256E to Arc.LEFT_DOWN,
    0x256F to Arc.LEFT_UP,
    0x2570 to Arc.RIGHT_UP,
)

/** Rounded corners as a quarter-circle tangent to the row it joins (the iOS `addArc(tangent1End:tangent2End:)`), stroked. */
private fun DrawScope.drawArc(arc: Arc, cell: Rect, color: Color, scale: Float) {
    val t = max(1f / scale, round(cell.width * 0.12f * scale) / scale)
    val cx = cell.center.x
    val cy = cell.center.y
    val radius = min(cell.width, cell.height) / 2
    val fromRight = arc == Arc.RIGHT_DOWN || arc == Arc.RIGHT_UP
    val down = arc == Arc.RIGHT_DOWN || arc == Arc.LEFT_DOWN

    // u1 runs from the start edge to the centre, u2 from the centre outward;
    // the arc is tangent to both, at distance `radius` from the centre.
    val u1 = Offset(if (fromRight) -1f else 1f, 0f)
    val u2 = Offset(0f, if (down) 1f else -1f)
    val start = Offset(if (fromRight) cell.right else cell.left, cy)
    val tangent1 = Offset(cx, cy)
    val end = Offset(cx, if (down) cell.bottom else cell.top)
    val arcStart = Offset(tangent1.x - u1.x * radius, tangent1.y - u1.y * radius)
    val center = Offset(tangent1.x - u1.x * radius + u2.x * radius, tangent1.y - u1.y * radius + u2.y * radius)
    val startAngle = Math.toDegrees(
        atan2(arcStart.y - center.y, arcStart.x - center.x).toDouble(),
    ).toFloat()
    val cross = u1.x * u2.y - u1.y * u2.x
    val sweep = if (cross >= 0) 90f else -90f

    val path = Path()
    path.moveTo(start.x, start.y)
    path.lineTo(arcStart.x, arcStart.y)
    path.arcTo(
        rect = Rect(center.x - radius, center.y - radius, center.x + radius, center.y + radius),
        startAngleDegrees = startAngle,
        sweepAngleDegrees = sweep,
        forceMoveTo = false,
    )
    path.lineTo(end.x, end.y)
    drawPath(path, color, style = Stroke(width = t))
}

/** Edges on device pixels, so cells that meet leave no seam. */
fun snapped(rect: Rect, scale: Float): Rect {
    val minX = round(rect.left * scale) / scale
    val minY = round(rect.top * scale) / scale
    val maxX = round(rect.right * scale) / scale
    val maxY = round(rect.bottom * scale) / scale
    val width = max(maxX - minX, 1f / scale)
    val height = max(maxY - minY, 1f / scale)
    return Rect(minX, minY, minX + width, minY + height)
}

private fun Color.withAlpha(alpha: Float): Color = copy(alpha = this.alpha * alpha)

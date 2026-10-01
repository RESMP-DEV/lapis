package dev.lapis.remote.ui.commandbar

import dev.lapis.remote.gateway.Key
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/** The payload of every bar key, as the gateway will receive it. */
class CommandBarKeysTest {

    @Test
    fun navigationKeysUseTheNamedKeyPath() {
        val expected = mapOf(
            "key-esc" to "escape",
            "key-tab" to "tab",
            "key-up" to "up",
            "key-down" to "down",
            "key-left" to "left",
            "key-right" to "right",
            "key-enter" to "enter",
            "key-backspace" to "backspace",
            "key-home" to "home",
            "key-end" to "end",
            "key-pgup" to "pageUp",
            "key-pgdn" to "pageDown",
        )
        expected.forEach { (tag, wire) ->
            val key = CommandBarKeys.fixed.single { it.testTag == tag }
            assertEquals(wire, key.payload.key, tag)
            assertEquals(0, key.payload.modifiers, tag)
            assertNull(key.payload.text, tag)
            assertNull(key.payload.paste, tag)
        }
    }

    @Test
    fun shiftTabIsTheTabKeyWithTheShiftModifier() {
        val shiftTab = CommandBarKeys.fixed.single { it.testTag == "key-shift-tab" }
        assertEquals(Key.TAB.wire, shiftTab.payload.key)
        assertEquals(1, shiftTab.payload.modifiers)
    }

    @Test
    fun ctrlChordsAreRawControlBytesThroughText() {
        val expected = mapOf(
            "key-ctrl-c" to 0x03,
            "key-ctrl-d" to 0x04,
            "key-ctrl-u" to 0x15,
            "key-ctrl-l" to 0x0C,
            "key-ctrl-r" to 0x12,
            "key-ctrl-z" to 0x1A,
            "key-ctrl-w" to 0x17,
        )
        expected.forEach { (tag, code) ->
            val chord = CommandBarKeys.fixed.single { it.testTag == tag }
            assertEquals(code, chord.payload.text!!.single().code, tag)
            assertNull(chord.payload.key, tag)
            assertNull(chord.payload.paste, tag)
        }
    }

    @Test
    fun everyChordIsItsLetterMinus0x40() {
        CommandBarKeys.fixed
            .filter { it.testTag.startsWith("key-ctrl-") }
            .forEach { chord ->
                val letter = chord.testTag.removePrefix("key-ctrl-").single()
                    .uppercaseChar()
                assertEquals(letter.code - 0x40, chord.payload.text!!.single().code)
            }
    }

    @Test
    fun snippetRunsAsPastePlusEnterInOneInput() {
        val payload = CommandBarKeys.snippet("count")
        assertEquals("count", payload.paste)
        assertEquals(Key.ENTER.wire, payload.key)
        assertNull(payload.text)
        assertNull(payload.modifiers)
        assertNull(payload.resize)
        assertNull(payload.wheel)
    }

    @Test
    fun everyKeyHasALabelAndAUniqueTag() {
        val tags = CommandBarKeys.fixed.map { it.testTag }
        assertEquals(tags.size, tags.toSet().size)
        CommandBarKeys.fixed.forEach { key -> assertTrue(key.label.isNotBlank()) }
    }
}

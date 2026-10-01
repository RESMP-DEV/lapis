package dev.lapis.remote.ui.commandbar

import dev.lapis.remote.gateway.Input
import dev.lapis.remote.gateway.Key

/**
 * The command bar's fixed key set: navigation keys travel the named-key
 * path (shift-tab as the tab key with the shift modifier) and Ctrl chords
 * go through TEXT as raw control bytes, matching the shipped iOS KeyBar
 * mechanism — TEXT passes through verbatim, so no gateway change exists
 * or is needed. Pure data so the payload of every key is unit-testable.
 */
data class CommandKey(
    val label: String,
    val testTag: String,
    val payload: Input,
)

object CommandBarKeys {

    /** Order per the design spec: esc, tab, shift-tab, arrows, enter,
     * backspace, home/end, page keys, then the Ctrl chords. */
    val fixed: List<CommandKey> = listOf(
        CommandKey("esc", "key-esc", Input.key(Key.ESCAPE)),
        CommandKey("tab", "key-tab", Input.key(Key.TAB)),
        CommandKey("⇧tab", "key-shift-tab", Input.key(Key.TAB, shift = true)),
        CommandKey("↑", "key-up", Input.key(Key.UP)),
        CommandKey("↓", "key-down", Input.key(Key.DOWN)),
        CommandKey("←", "key-left", Input.key(Key.LEFT)),
        CommandKey("→", "key-right", Input.key(Key.RIGHT)),
        CommandKey("⏎", "key-enter", Input.key(Key.ENTER)),
        CommandKey("⌫", "key-backspace", Input.key(Key.BACKSPACE)),
        CommandKey("home", "key-home", Input.key(Key.HOME)),
        CommandKey("end", "key-end", Input.key(Key.END)),
        CommandKey("pgup", "key-pgup", Input.key(Key.PAGE_UP)),
        CommandKey("pgdn", "key-pgdn", Input.key(Key.PAGE_DOWN)),
        CommandKey("^C", "key-ctrl-c", chord('C')),
        CommandKey("^D", "key-ctrl-d", chord('D')),
        CommandKey("^U", "key-ctrl-u", chord('U')),
        CommandKey("^L", "key-ctrl-l", chord('L')),
        CommandKey("^R", "key-ctrl-r", chord('R')),
        CommandKey("^Z", "key-ctrl-z", chord('Z')),
        CommandKey("^W", "key-ctrl-w", chord('W')),
    )

    /** Ctrl+letter is the bare control byte: the letter's code minus 0x40
     * (C=ETX 0x03, D=EOT 0x04, U=NAK 0x15, L=FF 0x0C, R=DC2 0x12,
     * Z=SUB 0x1A, W=ETB 0x17). Built arithmetically so the mapping rule
     * itself lives in the source rather than seven opaque literals.
     * Guarded at the boundary: the arithmetic is only a C0 byte for
     * 'A'..'Z', and any other letter would silently send the wrong
     * character through to the gateway. */
    private fun chord(letter: Char): Input {
        require(letter in 'A'..'Z') { "Ctrl-chord letter must be A..Z, got '$letter'" }
        return Input(text = (letter.code - 0x40).toChar().toString())
    }

    /** A snippet runs as paste plus Enter in one POST, the same shape the
     * composer's submit uses; only `paste` is re-encoded (bracketed-paste
     * sanitation) on the Mac. */
    fun snippet(snippet: String): Input =
        Input(paste = snippet, key = Key.ENTER.wire)
}

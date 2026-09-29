package dev.lapis.remote.gateway

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * Decodes the sanitized fixture files under src/test/resources/fixtures into
 * the DTOs. The fixtures mirror lapis_remote.py's payload shapes (hand-built
 * from the gateway source; no real hostnames, paths or users).
 */
class ModelsTest {
    private fun fixture(name: String): String =
        requireNotNull(javaClass.classLoader)
            .getResourceAsStream("fixtures/$name")!!
            .readBytes().toString(Charsets.UTF_8)

    @Test
    fun decodesWorkspaceListing() {
        val listing = gatewayJson.decodeFromString(
            WorkspaceListing.serializer(),
            fixture("workspace_listing.json"),
        )
        assertEquals("cat-work", listing.activeCategory)
        assertEquals(2, listing.categories.size)
        val work = listing.categories.first()
        assertEquals("work", work.name)
        assertEquals(2, work.agents.size)
        val local = work.agents[0]
        assertTrue(local.running)
        assertFalse(local.onPhone)
        assertEquals("", local.machine)
        assertEquals("~/dev/example", local.location)
        val remote = work.agents[1]
        assertEquals("devbox", remote.machine)
        assertEquals("devbox:~/lapis-notes", remote.place)
        assertEquals("devbox:~/lapis-notes", remote.location)
    }

    @Test
    fun olderGatewaysOmitMachineAndPlace() {
        val listing = gatewayJson.decodeFromString(
            WorkspaceListing.serializer(),
            fixture("workspace_listing_legacy.json"),
        )
        val agent = listing.categories.single().agents.single()
        assertNull(agent.machine)
        assertNull(agent.place)
        assertEquals("~/scratch", agent.location)
    }

    @Test
    fun decodesHarnessesAndDefaults() {
        val listing = gatewayJson.decodeFromString(
            HarnessesListing.serializer(),
            fixture("harnesses.json"),
        )
        assertEquals(2, listing.harnesses.size)
        val codex = listing.harnesses.first()
        assertTrue(codex.installed)
        val models = requireNotNull(codex.models)
        assertEquals("gpt-main", models.first().id)
        assertTrue(models.first().isDefault, "the wire key is \"default\"")
        assertEquals(2, requireNotNull(codex.modes).size)
        val missing = listing.harnesses[1]
        assertNull(missing.models)
        assertNull(missing.modes)
        assertEquals("codex", listing.defaults?.harness)
        assertEquals("~/dev", listing.defaults?.machines?.get("devbox"))
    }

    @Test
    fun decodesScreenFrameWithPositionalRuns() {
        val frame = gatewayJson.decodeFromString(
            ScreenFrame.serializer(),
            fixture("screen_frame.json"),
        )
        assertEquals(80, frame.columns)
        assertEquals(24, frame.rows)
        assertEquals(42L, frame.revision)
        assertTrue(frame.cursor.visible)
        assertEquals(3, frame.cursor.x)
        assertTrue(frame.alternateScreen)

        val shade = frame.lines[0].single()
        assertEquals("full-width shade", shade.text)
        assertNull(shade.foreground)
        assertEquals("#101010", shade.background)
        assertEquals(0, shade.flags)
        assertEquals(0, shade.column)
        assertEquals(80, shade.width)

        val allStyles = frame.lines[1][1]
        assertEquals(Run.BOLD + Run.ITALIC + Run.FAINT + Run.UNDERLINE + Run.STRIKE + Run.CURSOR, allStyles.flags)
        assertEquals(6, allStyles.column)
        assertEquals(41, allStyles.width)

        val legacy = frame.lines[2].single()
        assertNull(legacy.column, "older gateways stop after flags")
        assertNull(legacy.width)
        assertEquals(0, legacy.flags)

        val explicitNulls = frame.lines[3].single()
        assertEquals(8, explicitNulls.flags)
        assertNull(explicitNulls.foreground)

        assertEquals(
            frame.lines.joinToString("\n") { line -> line.joinToString("") { it.text } },
            frame.text,
        )
    }

    @Test
    fun runRequiresTextAndFlags() {
        // Swift's decoder throws on a run shorter than [text, fg, bg, flags];
        // the port refuses it the same way.
        val error = assertFailsWith<Exception> {
            gatewayJson.decodeFromString(
                Run.serializer(),
                "[\"text only\"]",
            )
        }
        assertTrue(error.message!!.contains("missing flags") || error.message!!.contains("missing text"))
    }

    @Test
    fun widthWithoutColumnRoundTripsInItsOwnPosition() {
        // A width-only run keeps fixed positions on the wire: the column
        // slot holds null, so width cannot land where column is read.
        val run = Run(text = "ab", foreground = null, background = null, flags = 0, column = null, width = 2)
        val encoded = gatewayJson.encodeToString(Run.serializer(), run)
        assertEquals("[\"ab\",null,null,0,null,2]", encoded)
        val back = gatewayJson.decodeFromString(Run.serializer(), encoded)
        assertEquals(run, back)
    }

    @Test
    fun decodesHistoryPage() {
        val page = gatewayJson.decodeFromString(
            HistoryPage.serializer(),
            fixture("history_page.json"),
        )
        assertEquals(7L, page.page)
        assertFalse(page.end)
        assertFalse(page.busy)
        assertEquals(80, page.columns)
        val lines = requireNotNull(page.lines)
        assertEquals(2, lines.size)
        assertEquals(1, lines[1][0].flags and Run.BOLD)
    }

    @Test
    fun encodesNewAgentLikeSwiftOmittingNulls() {
        val body = gatewayJson.encodeToString(
            NewAgent.serializer(),
            NewAgent(
                harness = "codex",
                directory = "~/dev/example",
                category = "cat-work",
                machine = null,
                model = null,
                mode = null,
            ),
        )
        assertFalse(body.contains("machine"), "Swift's JSONEncoder omits nil fields")
        assertFalse(body.contains("\"model\""))
        assertTrue(body.contains("\"harness\":\"codex\""))

        val withMachine = gatewayJson.encodeToString(
            NewAgent.serializer(),
            NewAgent("codex", "~/d", "cat", machine = "devbox", model = null, mode = null),
        )
        assertTrue(withMachine.contains("\"machine\":\"devbox\""))
    }

    @Test
    fun inputEncodingOmitsAbsentFields() {
        val paste = gatewayJson.encodeToString(
            Input.serializer(),
            Input(paste = "message", key = "enter"),
        )
        assertEquals("""{"paste":"message","key":"enter"}""", paste)

        val plainKey = gatewayJson.encodeToString(
            Input.serializer(),
            Input.key(Key.ENTER),
        )
        assertEquals("""{"key":"enter","modifiers":0}""", plainKey)

        val shiftTab = gatewayJson.encodeToString(
            Input.serializer(),
            Input.key(Key.TAB, shift = true),
        )
        assertEquals("""{"key":"tab","modifiers":1}""", shiftTab)

        val resize = gatewayJson.encodeToString(
            Input.serializer(),
            Input(resize = listOf(120, 40)),
        )
        assertEquals("""{"resize":[120,40]}""", resize)
    }

    @Test
    fun keyEnumMatchesWireNames() {
        assertEquals("pageUp", Key.PAGE_UP.wire)
        assertEquals("pageDown", Key.PAGE_DOWN.wire)
        assertEquals("escape", Key.ESCAPE.wire)
        assertEquals(13, Key.entries.size)
    }
}

package dev.lapis.remote.session

import dev.lapis.remote.gateway.Agent
import dev.lapis.remote.gateway.GatewayRequest
import dev.lapis.remote.gateway.GatewayResponse
import dev.lapis.remote.gateway.GatewayStream
import dev.lapis.remote.gateway.GatewayTransport
import dev.lapis.remote.gateway.Input
import dev.lapis.remote.gateway.LapisGateway
import java.util.Collections
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.fail
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking

/**
 * A scripted socket: canned stream lines, canned history pages consumed in
 * order (falling back to an empty page), recorded posts. The stream stays
 * open like a real one ([holdOpen]); a script that ends closes it. History
 * answers are consumed by whichever load asks first, so tests that want the
 * gather loop to stop give a full page (80+ rows).
 */
private class ScriptedTransport(
    var streamLines: List<String> = emptyList(),
    var holdOpen: Boolean = true,
) : GatewayTransport {
    val inputs: MutableList<String> = Collections.synchronizedList(mutableListOf())
    val historyRequests: MutableList<String> = Collections.synchronizedList(mutableListOf())
    val historyAnswers = ArrayDeque<String>()
    var streamCode = 200

    /** While true, each input POST waits for [releaseInputs]: the Mac has not answered yet. */
    @Volatile
    var stallInputs = false
    private val inputGate = java.util.concurrent.CountDownLatch(1)

    fun releaseInputs() {
        inputGate.countDown()
    }

    @Volatile
    private var open = true

    override fun call(request: GatewayRequest): GatewayResponse {
        val body: String = when {
            request.url.contains("/input") -> {
                if (stallInputs) inputGate.await()
                inputs += request.body!!.decodeToString()
                "{}"
            }
            request.url.contains("/history") -> {
                historyRequests += request.url
                synchronized(historyAnswers) { historyAnswers.removeFirstOrNull() } ?: EMPTY_PAGE
            }
            else -> "{}"
        }
        return GatewayResponse(200, body.encodeToByteArray())
    }

    override fun open(request: GatewayRequest): GatewayStream {
        open = true
        return GatewayStream(
            code = streamCode,
            lines = sequence {
                streamLines.forEach { yield(it) }
                // A live socket waits for events that have not happened yet.
                while (open && holdOpen) Thread.sleep(20)
            },
            close = { open = false },
        )
    }

    companion object {
        const val EMPTY_PAGE = """{"page":0,"message":"","end":false,"busy":false}"""
    }
}

private fun agent() = Agent(
    id = "a1",
    title = "repl",
    harness = "codex",
    directory = "~/dev/lapis",
    running = true,
    onPhone = false,
)

private fun frameEvent(revision: Long, columns: Int = 80, rows: Int = 24, text: String = "hi") = listOf(
    "event: frame",
    """data: {"revision": $revision, "columns": $columns, "rows": $rows,""" +
        """ "cursor": {"x": 0, "y": 0, "visible": true}, "alternateScreen": false,""" +
        """ "applicationCursor": false, "foreground": "#ffffff", "background": "#000000",""" +
        """ "lines": [[["$text", null, null, 0]]]}""",
)

private fun page(
    page: Long,
    rows: Int,
    offset: Int? = null,
    total: Int? = null,
    scrubbable: Boolean = false,
): String = buildString {
    append("""{"page": $page, "message": "", "end": false, "busy": false, "columns": 80, """)
    append(""""lines": [""")
    repeat(rows) { index ->
        if (index > 0) append(",")
        append("""[["row $page-$index", null, null, 0, 0, 12]]""")
    }
    append("]")
    if (offset != null && total != null) {
        append(""", "place": {"total": $total, "offset": $offset, "rows": $rows, "scrubbable": $scrubbable}""")
    }
    append("}")
}

/** Waits (real time; the scripted socket runs on real dispatchers) for a condition that must arrive. */
private fun await(what: String, timeoutMs: Long = 5_000, condition: () -> Boolean) {
    val deadline = System.currentTimeMillis() + timeoutMs
    while (System.currentTimeMillis() < deadline) {
        if (condition()) return
        Thread.sleep(10)
    }
    fail("timed out waiting for $what")
}

/**
 * AgentSession behavior against a scripted gateway: attachment lifecycle,
 * generation retirement's observable effects, the input queue contract, and
 * history paging including the empty-archive retry.
 */
class AgentSessionTest {
    private lateinit var scope: CoroutineScope
    private val sessions = mutableListOf<AgentSession>()

    @BeforeTest
    fun reset() {
        ScreenCache.clearAll()
        // The session confines its state to its scope's dispatcher (main in
        // the app); a single-threaded scope keeps that contract in tests.
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default.limitedParallelism(1))
    }

    @AfterTest
    fun tearDown() {
        sessions.forEach { it.close() }
        scope.cancel()
    }

    private fun session(transport: ScriptedTransport, columns: Int = 80, rows: Int = 24): AgentSession {
        val gateway = LapisGateway("http://mac:7349", transport, ioContext = Dispatchers.IO)
        val session = AgentSession(agent(), gateway, scope)
        sessions += session
        session.open(columns, rows)
        return session
    }

    private fun live(transport: ScriptedTransport): AgentSession {
        val session = session(transport)
        await("the live state") { session.state.value == AgentSession.State.Live }
        return session
    }

    @Test
    fun frameGoesLiveAndPrefetchesTheNewestPage() {
        val transport = ScriptedTransport(
            listOf("event: attached", """data: {"shared": true}""", "") +
                frameEvent(revision = 1),
        ).apply {
            historyAnswers += page(9, rows = 81, offset = 100, total = 500, scrubbable = true)
        }
        val session = live(transport)
        await("the history prefetch") { session.history.value.isNotEmpty() }

        assertEquals("hi", session.frame.value?.text)
        assertEquals(9, session.history.value.first().page)
        assertEquals(500, session.historyTotal.value)
        assertTrue(session.scrubbable.value)
        assertEquals(false, session.historyEnd.value)
        // The screen cache holds the last frame for the next attachment.
        assertEquals("hi", ScreenCache.frame("a1")?.text)
    }

    @Test
    fun resizeQueuedWhileConnectingFlushesAfterTheFirstFrame() {
        val transport = ScriptedTransport(frameEvent(revision = 1, columns = 80, rows = 24))
        val session = session(transport)
        session.resize(100, 30)

        await("the resize flush") { transport.inputs.isNotEmpty() }
        assertEquals(listOf("""{"resize":[100,30]}"""), transport.inputs.toList())
    }

    @Test
    fun statusExplainsItselfAndEndsWithoutReopen() {
        val transport = ScriptedTransport(
            frameEvent(revision = 1) + listOf(
                "",
                "event: status",
                """data: {"state": "ended", "message": ""}""",
            ),
        )
        val session = session(transport)
        await("the closed state") { session.state.value is AgentSession.State.Closed }

        val closed = session.state.value as AgentSession.State.Closed
        assertEquals("This agent has ended.", closed.reason)
        assertEquals(false, closed.reopen)
    }

    @Test
    fun aQuietStreamEndIsTheMacClosingTheConnection() {
        val transport = ScriptedTransport(frameEvent(revision = 1), holdOpen = false)
        val session = session(transport)
        await("the closed state") { session.state.value is AgentSession.State.Closed }

        val closed = session.state.value as AgentSession.State.Closed
        assertEquals("The Mac closed the connection.", closed.reason)
        assertEquals(true, closed.reopen)
    }

    @Test
    fun aRefusedStreamClosesWithReopen() {
        val transport = ScriptedTransport(listOf("""{"error": "nope"}"""), holdOpen = false)
            .apply { streamCode = 503 }
        val session = session(transport)
        await("the closed state") { session.state.value is AgentSession.State.Closed }

        val closed = session.state.value as AgentSession.State.Closed
        assertEquals("nope", closed.reason)
        assertEquals(true, closed.reopen)
    }

    @Test
    fun reopeningAttachesAgainWithAFreshStream() {
        val transport = ScriptedTransport(frameEvent(revision = 1), holdOpen = false)
        val session = session(transport)
        await("the closed state") { session.state.value is AgentSession.State.Closed }

        transport.streamLines = frameEvent(revision = 2, text = "again")
        transport.holdOpen = true
        session.open(80, 24)
        await("the live state") { session.state.value == AgentSession.State.Live }
        await("the second frame") { session.frame.value?.text == "again" }
    }

    @Test
    fun submitPastesTheMessageAndPressesEnter() {
        val transport = ScriptedTransport(frameEvent(revision = 1))
        val session = live(transport)

        session.submit("git status")
        session.submit("")
        await("both input posts") { transport.inputs.size >= 2 }
        assertEquals(
            listOf("""{"paste":"git status","key":"enter"}""", """{"key":"enter","modifiers":0}"""),
            transport.inputs.toList(),
        )
    }

    @Test
    fun inputOverflowsAt128WithAVisibleNotice() {
        val transport = ScriptedTransport(frameEvent(revision = 1))
        val session = live(transport)

        // The Mac stalls on the first key, so the queue fills before it drains.
        transport.stallInputs = true
        repeat(130) { index -> session.send(Input(text = "k$index")) }
        await("the overflow notice") { session.notice.value != null }
        assertEquals(0, transport.inputs.size)
        assertEquals(
            "Input is still being sent. Wait for the Mac before typing more.",
            session.notice.value,
        )

        transport.releaseInputs()
        await("the queue to drain") { session.inputPending.value == false }
        assertEquals(128, transport.inputs.size)
    }

    @Test
    fun anEmptyArchiveIsRetriedNotTreatedAsTheStart() {
        val transport = ScriptedTransport(frameEvent(revision = 1))
        val session = live(transport)
        await("the prefetch ask") { transport.historyRequests.size >= 1 }
        assertEquals(false, session.historyEnd.value)

        runBlocking { session.loadOlder() }
        await("the second ask") { transport.historyRequests.size >= 2 }

        // The second ask waited out the one-per-second spacing, not the archive's start.
        assertEquals(2, transport.historyRequests.size)
        assertEquals(false, session.historyEnd.value)
    }

    @Test
    fun loadingGathersRowsUntilTheArchiveEnds() {
        val transport = ScriptedTransport(frameEvent(revision = 1)).apply {
            // A full prefetch page stops its gather loop; the explicit load
            // gathers the next page, then meets the archive's start.
            historyAnswers += page(9, rows = 81, offset = 100, total = 500)
            historyAnswers += page(8, rows = 2, offset = 98, total = 500)
        }
        val session = live(transport)
        await("the prefetch page") { session.history.value.isNotEmpty() }
        assertEquals(listOf(9L), session.history.value.map { it.page })

        runBlocking { session.loadOlder() }

        assertEquals(listOf(8L, 9L), session.history.value.map { it.page })
        assertEquals(true, session.historyEnd.value)
    }

    @Test
    fun aBusyPageIsRetriedAfterAQuarterSecond() {
        val transport = ScriptedTransport(frameEvent(revision = 1)).apply {
            historyAnswers += """{"page": 0, "message": "", "end": false, "busy": true}"""
            historyAnswers += page(9, rows = 81)
        }
        val session = session(transport)
        await("the busy retry to land") { session.history.value.isNotEmpty() }

        assertEquals(2, transport.historyRequests.size)
    }

    @Test
    fun jumpLandsOnARowAloneAndLeavesAGap() {
        val transport = ScriptedTransport(frameEvent(revision = 1)).apply {
            historyAnswers += page(9, rows = 81, offset = 100, total = 500, scrubbable = true)
            historyAnswers += page(4, rows = 40, offset = 900, total = 1000, scrubbable = true)
        }
        val session = live(transport)
        await("the prefetch page") { session.history.value.isNotEmpty() }

        // Half of the 500 kept rows: row 250.
        runBlocking { session.jump(0.5) }

        assertTrue(transport.historyRequests.any { it.endsWith("at=250") })
        assertEquals(listOf(4L), session.history.value.map { it.page })
        assertEquals(false, session.historyEnd.value)
        assertEquals(true, session.gapAfter.value)
        assertEquals(session.history.value.single().cacheId, session.jumpedTo.value)
    }
}

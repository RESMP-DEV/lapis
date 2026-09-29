package dev.lapis.remote.gateway

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.test.runTest

/** A deterministic in-memory transport: records requests, replays canned answers. */
private class FakeTransport(
    var lines: Sequence<String> = emptySequence(),
    var streamCode: Int = 200,
    // Last, so a trailing lambda in tests configures the responder.
    var respond: (GatewayRequest) -> GatewayResponse = {
        GatewayResponse(200, "{}".encodeToByteArray())
    },
) : GatewayTransport {
    val requests = mutableListOf<GatewayRequest>()

    override fun call(request: GatewayRequest): GatewayResponse {
        requests += request
        return respond(request)
    }

    override fun open(request: GatewayRequest): GatewayStream {
        requests += request
        return GatewayStream(
            code = streamCode,
            lines = lines,
            close = { closed = true },
        )
    }

    var closed = false
        private set
}

/**
 * Gateway behavior over the fake transport: header contract, endpoint paths,
 * error mapping, and the SSE flow. These are the observable wire facts the
 * gateway source (lapis_remote.py) replies to.
 */
class LapisGatewayTest {
    private fun gateway(transport: FakeTransport) =
        LapisGateway("http://mac:7349", transport)

    private fun listingJson(): String =
        requireNotNull(javaClass.classLoader)
            .getResourceAsStream("fixtures/workspace_listing.json")!!
            .readBytes().toString(Charsets.UTF_8)

    @Test
    fun agentsCarriesClientAndCacheHeaders() = runTest {
        val transport = FakeTransport {
            GatewayResponse(200, listingJson().encodeToByteArray())
        }
        val listing = gateway(transport).agents()
        assertEquals("cat-work", listing.activeCategory)
        val request = transport.requests.single()
        assertEquals("GET", request.method)
        assertEquals("http://mac:7349/api/agents", request.url)
        assertEquals("android", request.headers["X-Lapis-Client"])
        assertEquals("no-cache", request.headers["Cache-Control"])
        assertNull(request.body)
    }

    @Test
    fun refusedWithErrorBodySurfacesTheMessage() = runTest {
        val transport = FakeTransport {
            GatewayResponse(404, """{"error": "Not found"}""".encodeToByteArray())
        }
        val error = assertFailsWith<GatewayError.Refused> { gateway(transport).agents() }
        assertEquals(404, error.code)
        assertEquals("Not found", error.bodyMessage)
        assertEquals("Not found", error.message)
    }

    @Test
    fun refusedWithoutErrorBodyExplainsTheCode() = runTest {
        val transport = FakeTransport {
            GatewayResponse(503, "service unavailable".encodeToByteArray())
        }
        val error = assertFailsWith<GatewayError.Refused> { gateway(transport).agents() }
        assertEquals(503, error.code)
        assertEquals("", error.bodyMessage)
        assertEquals("The Mac answered 503.", error.message)
    }

    @Test
    fun malformedAnswerIsUnreadable() = runTest {
        val transport = FakeTransport {
            GatewayResponse(200, "<html>".encodeToByteArray())
        }
        assertFailsWith<GatewayError.Unreadable> { gateway(transport).agents() }
    }

    @Test
    fun startPostsTheAgentBody() = runTest {
        val transport = FakeTransport {
            GatewayResponse(200, """{"id": "new-1", "updating": true}""".encodeToByteArray())
        }
        val started = gateway(transport).start(
            NewAgent(harness = "codex", directory = "~/d", category = "cat"),
        )
        assertEquals("new-1", started.id)
        assertTrue(started.updating)
        val request = transport.requests.single()
        assertEquals("POST", request.method)
        assertEquals("http://mac:7349/api/agents", request.url)
        assertEquals("application/json", request.contentType)
        val body = request.body!!.toString(Charsets.UTF_8)
        assertTrue(body.contains("\"harness\":\"codex\""))
    }

    @Test
    fun createCategoryPostsNameAndReturnsId() = runTest {
        val transport = FakeTransport {
            GatewayResponse(200, """{"id": "cat-new"}""".encodeToByteArray())
        }
        assertEquals("cat-new", gateway(transport).createCategory("spare"))
        val body = transport.requests.single().body!!.toString(Charsets.UTF_8)
        assertEquals("""{"name":"spare"}""", body)
    }

    @Test
    fun closePostsTheAgentRoute() = runTest {
        val transport = FakeTransport()
        gateway(transport).close(agent = "agent-9")
        val request = transport.requests.single()
        assertEquals("POST", request.method)
        assertEquals("http://mac:7349/api/agents/agent-9/close", request.url)
    }

    @Test
    fun machinesUnwrapsTheListing() = runTest {
        val transport = FakeTransport {
            GatewayResponse(
                200,
                """{"machines": [{"name": "devbox", "uses": 3, "available": true}]}"""
                    .encodeToByteArray(),
            )
        }
        val machines = gateway(transport).machines()
        assertEquals(listOf(Machine("devbox", 3, true)), machines)
    }

    @Test
    fun foldersOmitsEmptyMachineAndMissingHave() = runTest {
        val transport = FakeTransport {
            GatewayResponse(
                200,
                """{"version": "v1", "unchanged": true}""".encodeToByteArray(),
            )
        }
        val payload = gateway(transport).folders(machine = "", have = null)
        assertTrue(payload.unchanged == true)
        val request = transport.requests.single()
        assertEquals("http://mac:7349/api/folders", request.url, "no query for this-Mac defaults")
        assertEquals(45L, request.timeoutSeconds, "ssh-backed first reports get the long timeout")

        gateway(transport).folders(machine = "dev box", have = "v1")
        assertEquals(
            "http://mac:7349/api/folders?machine=dev%20box&have=v1",
            transport.requests[1].url,
        )
    }

    @Test
    fun screenAndHistoryBuildTheirRoutes() = runTest {
        val frameJson =
            """{"columns": 80, "rows": 24, "cursor": {"x": 0, "y": 0, "visible": false}, """ +
                """"alternateScreen": false, "applicationCursor": false, "foreground": "#fff", """ +
                """"background": "#000", "lines": []}"""
        val pageJson = """{"page": 3, "message": "", "end": true, "busy": false}"""
        val transport = FakeTransport(
            respond = { request ->
                val body = when {
                    request.url.endsWith("/screen") -> frameJson
                    else -> pageJson
                }
                GatewayResponse(200, body.encodeToByteArray())
            },
        )
        val gateway = LapisGateway("http://mac:7349", transport)
        gateway.screen(agent = "a1")
        gateway.history(agent = "a1", before = 5)
        gateway.history(agent = "a1", after = 9)
        assertEquals("http://mac:7349/api/agents/a1/screen", transport.requests[0].url)
        assertEquals("http://mac:7349/api/agents/a1/history?before=5", transport.requests[1].url)
        assertEquals("http://mac:7349/api/agents/a1/history?after=9", transport.requests[2].url)
    }

    @Test
    fun sendPostsInputPayload() = runTest {
        val transport = FakeTransport()
        gateway(transport).send(Input.key(Key.ESCAPE), agent = "a1")
        val request = transport.requests.single()
        assertEquals("http://mac:7349/api/agents/a1/input", request.url)
        assertEquals("""{"key":"escape","modifiers":0}""", request.body!!.toString(Charsets.UTF_8))
    }

    @Test
    fun streamParsesFixtureLinesIntoEvents() = runTest {
        val streamLines = requireNotNull(javaClass.classLoader)
            .getResourceAsStream("fixtures/stream_events.txt")!!
            .readBytes().toString(Charsets.UTF_8)
            .split("\n")
        val transport = FakeTransport(lines = streamLines.asSequence())
        val events = gateway(transport).stream(agent = "a1", columns = 120, rows = 40).toList()
        assertEquals(3, events.size)
        assertTrue(events[0] is StreamEvent.Attached)
        assertTrue(events[1] is StreamEvent.Frame)
        assertTrue(events[2] is StreamEvent.Status)
        assertEquals(
            "http://mac:7349/api/agents/a1/stream?columns=120&rows=40",
            transport.requests.single().url,
        )
        assertTrue(transport.closed, "exhausting the stream closes the connection")
    }

    @Test
    fun streamRefusalReadsTheErrorBodyAndFails() = runTest {
        val transport = FakeTransport(
            streamCode = 503,
            lines = sequenceOf("""{"error": "overloaded"}"""),
        )
        val error = assertFailsWith<GatewayError.Refused> {
            gateway(transport).stream(agent = "a1", columns = 80, rows = 24).toList()
        }
        assertEquals(503, error.code)
        assertEquals("overloaded", error.bodyMessage)
        assertTrue(transport.closed, "the refused stream is closed before the flow fails")
    }
}

package dev.lapis.remote.gateway

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.fail

/**
 * SSE line parsing from the fixture of streamed lines, mirroring what
 * lapis_remote.py writes: `event: {name}\ndata: {json}\n\n` plus `: ping`
 * keepalive comments.
 */
class SseParserTest {
    private fun streamFixture(): List<String> =
        javaClass.classLoader.getResourceAsStream("fixtures/stream_events.txt")!!
            .readBytes().toString(Charsets.UTF_8)
            .split("\n")

    @Test
    fun parsesAttachedFrameAndStatusFromFixtureAndSkipsTheRest() {
        val parser = SseEventParser()
        val events = streamFixture().mapNotNull { parser.feed(it) }
        assertEquals(3, events.size, "keepalive comments and unknown events yield nothing")

        val attached = events[0] as StreamEvent.Attached
        assertTrue(attached.attached.shared)

        val frame = events[1] as StreamEvent.Frame
        assertEquals(4, frame.frame.columns)
        assertEquals("hi", frame.frame.text)
        assertTrue(
            frame.json.startsWith("""{"revision": 1"""),
            "the Frame keeps the exact JSON it came from, as iOS sends with captures",
        )

        val status = events[2] as StreamEvent.Status
        assertEquals("replaced", status.status.state)
    }

    @Test
    fun dataBeforeAnyEventNameIsIgnored() {
        val parser = SseEventParser()
        assertNull(parser.feed("data: {\"orphan\": 1}"))
    }

    @Test
    fun keepaliveCommentsAndBlankLinesAreIgnored() {
        val parser = SseEventParser()
        assertNull(parser.feed(": ping"))
        assertNull(parser.feed(""))
    }

    @Test
    fun unknownEventNamesAreSkipped() {
        val parser = SseEventParser()
        assertNull(parser.feed("event: future-event"))
        assertNull(parser.feed("data: {\"x\": 1}"))
    }

    @Test
    fun malformedKnownPayloadFailsTheStream() {
        val parser = SseEventParser()
        parser.feed("event: attached")
        try {
            parser.feed("data: {not json")
            fail("a malformed known event must fail the stream, as the iOS decode does")
        } catch (expected: Exception) {
            assertTrue(expected !is kotlinx.coroutines.CancellationException)
        }
    }
}

package dev.lapis.remote.gateway

// Server-sent events from `api/agents/{id}/stream`: the gateway writes
// `event: {name}\ndata: {json}\n\n` (lapis_remote.py:1733) and keepalive
// comment lines `: ping` every 10 s. This is the port of the line loop in
// Gateway.stream (Gateway.swift): event names switch the decoder, unknown
// names and every non-event line are ignored, and a malformed payload of a
// known event fails the stream.

class SseEventParser(
    private val json: kotlinx.serialization.json.Json = gatewayJson,
) {
    private var name = ""

    /** Feeds one raw line; returns the decoded event or null when the line produced none. */
    fun feed(line: String): StreamEvent? {
        if (line.startsWith("event: ")) {
            name = line.substring("event: ".length)
            return null
        }
        if (!line.startsWith("data: ")) {
            // Comment keepalives, blank separators, anything else.
            return null
        }
        val payload = line.substring("data: ".length)
        return when (name) {
            "attached" -> StreamEvent.Attached(
                json.decodeFromString(Attached.serializer(), payload),
            )
            "frame" -> StreamEvent.Frame(
                json.decodeFromString(ScreenFrame.serializer(), payload),
                payload,
            )
            "status" -> StreamEvent.Status(
                json.decodeFromString(StreamStatus.serializer(), payload),
            )
            else -> null
        }
    }
}

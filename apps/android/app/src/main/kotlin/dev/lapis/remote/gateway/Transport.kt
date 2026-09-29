package dev.lapis.remote.gateway

import java.util.concurrent.TimeUnit
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody

/**
 * The HTTP seam under [LapisGateway]: protocol headers and URLs live in the
 * gateway (and are asserted by the unit tests through fakes), socket and
 * timeout policy live here. One method per gateway behavior so a fake can
 * gate, record and replay answers deterministically.
 */
data class GatewayRequest(
    val method: String,
    val url: String,
    val body: ByteArray? = null,
    val contentType: String? = null,
    val headers: Map<String, String> = emptyMap(),
    /** Per-call read-timeout override in seconds; null uses the transport default. */
    val timeoutSeconds: Long? = null,
)

data class GatewayResponse(
    val code: Int,
    val bytes: ByteArray,
) {
    fun text(): String = bytes.toString(Charsets.UTF_8)
}

/**
 * An opened SSE connection: the status is known once [open] returns, and
 * [lines] reads the body lazily. [close] releases the underlying response;
 * callers must close even when they abandon [lines] early.
 */
class GatewayStream(
    val code: Int,
    val lines: Sequence<String>,
    private val close: () -> Unit,
) {
    fun closeStream() = close()
}

interface GatewayTransport {
    /** Blocking request/response. */
    fun call(request: GatewayRequest): GatewayResponse

    /** Blocking open of a streamed body; [GatewayStream.lines] blocks per line. */
    fun open(request: GatewayRequest): GatewayStream
}

/**
 * OkHttp-backed transport mirroring the iOS URLSessions in Gateway.swift:
 * plain calls answer or fail within 15 s; streams have a 40 s read timeout
 * (the gateway pings every 10 s, so a quiet screen is not a timeout) and no
 * overall call timeout.
 */
class OkHttpTransport : GatewayTransport {
    private val requests: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .writeTimeout(15, TimeUnit.SECONDS)
        .callTimeout(0, TimeUnit.SECONDS)
        .build()

    private val streams: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(40, TimeUnit.SECONDS)
        .writeTimeout(15, TimeUnit.SECONDS)
        .callTimeout(0, TimeUnit.SECONDS)
        .build()

    private fun GatewayRequest.build(client: OkHttpClient): Request {
        val builder = Request.Builder().url(url)
        val body = body?.toRequestBody(contentType?.toMediaType())
        when (method) {
            "POST" -> builder.post(
                body ?: ByteArray(0).toRequestBody(null),
            )
            else -> builder.get()
        }
        headers.forEach { (name, value) -> builder.header(name, value) }
        return builder.build()
    }

    override fun call(request: GatewayRequest): GatewayResponse {
        val client = request.timeoutSeconds
            ?.let { requests.newBuilder().readTimeout(it, TimeUnit.SECONDS).build() }
            ?: requests
        client.newCall(request.build(client)).execute().use { response ->
            return GatewayResponse(response.code, response.body.bytes())
        }
    }

    override fun open(request: GatewayRequest): GatewayStream {
        val call = streams.newCall(request.build(streams))
        val response = call.execute()
        val source = response.body.source()
        // One SSE line carries a whole serialized screen frame; the gateway
        // caps a frame at 8 MiB (MAX_FRAME in lapis_remote.py), so a line is
        // bounded at that plus envelope slack. Without the bound, a peer
        // could exhaust memory with one unbounded line.
        val maxLine = 8 * 1024 * 1024 + 4096
        val sequence = sequence {
            try {
                while (true) {
                    val line = try {
                        source.readUtf8LineStrict(maxLine.toLong())
                    } catch (end: java.io.EOFException) {
                        // okio raises EOFException both for a stream that
                        // ended at a line boundary (nothing buffered) and for
                        // a line that outgrew the limit (bytes still buffered);
                        // only the first is a clean end, the second is a
                        // protocol violation and must not look like one.
                        if (source.exhausted()) break else throw java.io.IOException(
                            "streamed line exceeded $maxLine bytes",
                            end,
                        )
                    }
                    yield(line)
                }
            } finally {
                source.close()
                response.close()
            }
        }
        return GatewayStream(
            code = response.code,
            lines = sequence,
            close = {
                source.close()
                response.close()
            },
        )
    }
}

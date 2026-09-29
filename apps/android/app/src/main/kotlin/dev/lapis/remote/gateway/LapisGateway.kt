package dev.lapis.remote.gateway

import java.net.URLEncoder
import kotlin.coroutines.CoroutineContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.withContext
import kotlinx.serialization.KSerializer

/**
 * The lapis gateway on the Mac (apps/remote/lapis_remote.py), reached over
 * Tailscale or ZeroTier. It admits only the Mac owner's devices, so there is
 * nothing to sign in to here. Port of apps/ios/Lapis/Gateway.swift.
 */
class LapisGateway(
    /** Normalized base URL, always scheme://host:port with no path. */
    val base: String,
    private val transport: GatewayTransport,
    private val ioContext: CoroutineContext = Dispatchers.IO,
) {
    companion object {
        const val DEFAULT_PORT = 7349
        const val CLIENT_HEADER = "X-Lapis-Client"
        const val CLIENT_VALUE = "android"

        /**
         * URL normalization identical to `Gateway.init(host:)`: the value is
         * trimmed, an empty value is invalid, `http://` is the default scheme,
         * port 7349 is the default, only http/https are supported, and any
         * path or query on the input is discarded.
         *
         * @throws GatewayError.InvalidHost on an empty or unparseable host
         * @throws GatewayError.UnsupportedScheme on any scheme but http/https
         */
        fun normalizeBase(host: String): String {
            val text = host.trim()
            if (text.isEmpty()) throw GatewayError.InvalidHost
            val withScheme = if (!text.contains("://")) "http://$text" else text
            val schemeEnd = withScheme.indexOf("://")
            val scheme = withScheme.take(schemeEnd).lowercase()
            if (scheme != "http" && scheme != "https") {
                throw GatewayError.UnsupportedScheme(withScheme.take(schemeEnd))
            }
            // Everything up to the first path, query or fragment separator is
            // the authority; the rest is dropped (Swift sets parts.path = "").
            val authority = withScheme.substring(schemeEnd + 3)
                .takeWhile { it != '/' && it != '?' && it != '#' }
            if (authority.isEmpty() || authority.any { it.isWhitespace() || it.isISOControl() }) {
                throw GatewayError.InvalidHost
            }
            // Split optional userinfo, then host[:port] with bracketed IPv6.
            // URLComponents keeps userinfo in the resulting URL; mirror that.
            val hostPort = authority.substringAfterLast('@', authority)
            val userinfo = if (hostPort != authority) {
                authority.removeSuffix(hostPort)
            } else {
                ""
            }
            val host: String
            val port: Int
            if (hostPort.startsWith("[")) {
                val end = hostPort.indexOf(']')
                if (end < 0) throw GatewayError.InvalidHost
                host = hostPort.take(end + 1)
                val portText = hostPort.substring(end + 1).removePrefix(":")
                port = parsePort(portText)
            } else {
                val colon = hostPort.lastIndexOf(':')
                if (colon < 0) {
                    host = hostPort
                    port = DEFAULT_PORT
                } else {
                    host = hostPort.take(colon)
                    port = parsePort(hostPort.substring(colon + 1))
                }
            }
            if (host.isEmpty()) throw GatewayError.InvalidHost
            return "$scheme://$userinfo$host:$port"
        }

        private fun parsePort(text: String): Int {
            if (text.isEmpty()) return DEFAULT_PORT
            val port = text.toIntOrNull() ?: throw GatewayError.InvalidHost
            if (port !in 1..65535) throw GatewayError.InvalidHost
            return port
        }

        fun build(
            host: String,
            transport: GatewayTransport = OkHttpTransport(),
            ioContext: CoroutineContext = Dispatchers.IO,
        ): LapisGateway = LapisGateway(normalizeBase(host), transport, ioContext)
    }

    // Requests carry the same headers as the iOS client, with the platform tag
    // "android": the gateway refuses clients without X-Lapis-Client, and
    // Cache-Control: no-cache is the port of reloadIgnoringLocalCacheData.
    private fun request(
        path: String,
        query: String? = null,
        method: String = "GET",
        body: ByteArray? = null,
        timeoutSeconds: Long? = null,
    ): GatewayRequest = GatewayRequest(
        method = method,
        url = buildString {
            append(base)
            append('/')
            append(path)
            if (query != null) {
                append('?')
                append(query)
            }
        },
        body = body,
        contentType = if (body != null) "application/json" else null,
        headers = mapOf(CLIENT_HEADER to CLIENT_VALUE, "Cache-Control" to "no-cache"),
        timeoutSeconds = timeoutSeconds,
    )

    private fun query(vararg pairs: Pair<String, String?>): String? =
        pairs.mapNotNull { (name, value) ->
            value?.let { "$name=${encodeQuery(it)}" }
        }.takeIf { it.isNotEmpty() }?.joinToString("&")

    private fun encodeQuery(value: String): String =
        URLEncoder.encode(value, Charsets.UTF_8).replace("+", "%20")

    /** Runs the blocking transport call off the caller's thread. */
    private suspend fun execute(request: GatewayRequest): GatewayResponse =
        withContext(ioContext) { transport.call(request) }

    /** Non-200 answers carry the gateway's `error` text when they carry JSON. */
    private fun refused(response: GatewayResponse): GatewayError.Refused {
        val body = runCatching {
            gatewayJson.decodeFromString(
                kotlinx.serialization.serializer<Map<String, String>>(),
                response.text(),
            )
        }.getOrNull()
        return GatewayError.Refused(response.code, body?.get("error") ?: "")
    }

    private suspend fun <T> get(
        path: String,
        query: String? = null,
        serializer: KSerializer<T>,
        timeoutSeconds: Long? = null,
    ): T {
        val response = execute(request(path, query, timeoutSeconds = timeoutSeconds))
        if (response.code != 200) throw refused(response)
        return decode(serializer, response)
    }

    private suspend fun post(path: String, body: ByteArray? = null): GatewayResponse {
        val response = execute(request(path, method = "POST", body = body))
        if (response.code != 200) throw refused(response)
        return response
    }

    private fun <T> decode(serializer: KSerializer<T>, response: GatewayResponse): T =
        try {
            gatewayJson.decodeFromString(serializer, response.text())
        } catch (_: kotlinx.serialization.SerializationException) {
            // A malformed answer is unreadable, not a crash; mirrors the iOS
            // decode failures surfacing as one user-facing message.
            throw GatewayError.Unreadable
        }

    suspend fun agents(): WorkspaceListing =
        get("api/agents", serializer = WorkspaceListing.serializer())

    suspend fun harnesses(): HarnessesListing =
        get("api/harnesses", serializer = HarnessesListing.serializer())

    suspend fun start(agent: NewAgent): StartedAgent {
        val response = post(
            "api/agents",
            gatewayJson.encodeToString(NewAgent.serializer(), agent).encodeToByteArray(),
        )
        return decode(StartedAgent.serializer(), response)
    }

    /** A new category on the Mac; its id. */
    suspend fun createCategory(name: String): String {
        val response = post(
            "api/categories",
            gatewayJson.encodeToString(CategoryName.serializer(), CategoryName(name))
                .encodeToByteArray(),
        )
        return decode(MadeCategory.serializer(), response).id
    }

    /** Ends the agent on the Mac, as Command-W does there. */
    suspend fun close(agent: String) {
        post("api/agents/$agent/close")
    }

    suspend fun machines(): List<Machine> =
        get("api/machines", serializer = MachineListing.serializer()).machines

    /** Another machine's first report is read over ssh, so allow it time. */
    suspend fun folders(machine: String, have: String?): FolderPayload =
        get(
            "api/folders",
            query("machine" to machine.takeIf { it.isNotEmpty() }, "have" to have),
            FolderPayload.serializer(),
            timeoutSeconds = 45,
        )

    /** The agent's current screen, read without resizing it. */
    suspend fun screen(agent: String): ScreenFrame =
        get("api/agents/$agent/screen", serializer = ScreenFrame.serializer())

    suspend fun send(input: Input, agent: String) {
        post(
            "api/agents/$agent/input",
            gatewayJson.encodeToString(Input.serializer(), input).encodeToByteArray(),
        )
    }

    /** The page before [before] (0: the newest), or with [after], the page after it. */
    suspend fun history(agent: String, before: Long = 0, after: Long? = null): HistoryPage {
        val item = if (after != null) "after=$after" else "before=$before"
        return get("api/agents/$agent/history", item, HistoryPage.serializer())
    }

    /**
     * Server-sent events; one cold flow per collection, mirroring the iOS
     * single-consumer AsyncThrowingStream. A non-200 open reads up to 4096
     * bytes to surface the gateway's refusal, then fails the flow.
     */
    fun stream(agent: String, columns: Int, rows: Int): Flow<StreamEvent> = flow {
        // Every blocking step hops to ioContext, but emissions stay on the
        // collecting coroutine (the flow invariant); the collector's thread
        // never blocks on the stream body.
        val opened = withContext(ioContext) {
            transport.open(
                request(
                    "api/agents/$agent/stream",
                    query("columns" to columns.toString(), "rows" to rows.toString()),
                ),
            )
        }
        try {
            if (opened.code != 200) {
                val body = withContext(ioContext) {
                    val text = StringBuilder()
                    for (line in opened.lines) {
                        text.appendLine(line)
                        if (text.length > 4096) break
                    }
                    text
                }
                val message = runCatching {
                    gatewayJson.decodeFromString<Map<String, String>>(body.toString())
                }.getOrNull()?.get("error") ?: ""
                throw GatewayError.Refused(opened.code, message)
            }
            val parser = SseEventParser(gatewayJson)
            val lines = opened.lines.iterator()
            while (true) {
                val line = withContext(ioContext) {
                    if (lines.hasNext()) lines.next() else null
                } ?: break
                parser.feed(line)?.let { emit(it) }
            }
        } finally {
            // Cancellation must not strand the connection: the close runs on
            // ioContext under a NonCancellable job.
            withContext(ioContext + NonCancellable) { opened.closeStream() }
        }
    }
}

/** Response of `api/harnesses`: the CLIs and the workspace's newAgent defaults. */
@kotlinx.serialization.Serializable
data class HarnessesListing(
    val harnesses: List<Harness>,
    val defaults: AgentDefaults? = null,
)

@kotlinx.serialization.Serializable
internal data class MachineListing(val machines: List<Machine>)

@kotlinx.serialization.Serializable
internal data class CategoryName(val name: String)

@kotlinx.serialization.Serializable
internal data class MadeCategory(val id: String)

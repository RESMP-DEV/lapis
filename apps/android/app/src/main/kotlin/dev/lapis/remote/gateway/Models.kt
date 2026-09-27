package dev.lapis.remote.gateway

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonPrimitive

// DTOs mirroring apps/ios/Lapis/Gateway.swift exactly; the wire contract is
// apps/remote/lapis_remote.py. Older gateways omit fields the Swift models
// declare optional; those are optional here too.

/** Shared JSON behavior: unknown keys are ignored (Swift Codable default), and
 *  encoding omits nulls so request bodies match Swift's JSONEncoder output. */
val gatewayJson: Json = Json {
    ignoreUnknownKeys = true
    explicitNulls = false
    encodeDefaults = false
}

@Serializable
data class WorkspaceListing(
    val categories: List<AgentCategory>,
    val activeCategory: String,
)

@Serializable
data class AgentCategory(
    val id: String,
    val name: String,
    val agents: List<Agent> = emptyList(),
)

@Serializable
data class Agent(
    val id: String,
    val title: String,
    val harness: String,
    val directory: String,
    val running: Boolean,
    val onPhone: Boolean,
    /** "" on this Mac; the ssh host otherwise. Older gateways send neither. */
    val machine: String? = null,
    /** "~/dev/infinity" here, "devbox:~/lapis" on another machine. */
    val place: String? = null,
) {
    val location: String get() = place ?: directory
}

/** An agent CLI the Mac can start, as its lapis reports it. */
@Serializable
data class Harness(
    val id: String,
    val name: String,
    val installed: Boolean,
    /** Models the CLI lists (its default first) and the approval modes it has; older Macs send none. */
    val models: List<ModelChoice>? = null,
    val modes: List<AgentMode>? = null,
)

@Serializable
data class ModelChoice(
    val id: String,
    val name: String,
    /** The CLI's own default: started without naming a model. */
    @SerialName("default") val isDefault: Boolean,
)

@Serializable
data class AgentMode(
    val id: String,
    val name: String,
)

/** lapis.json's newAgent defaults: the CLI, and the folder on this Mac and on each ssh machine ("~/dev" style). */
@Serializable
data class AgentDefaults(
    val harness: String? = null,
    val folder: String? = null,
    val mode: String? = null,
    val machines: Map<String, String>? = null,
)

/** An agent to start, as a new tab in [category] on the Mac: on the Mac, or over ssh on [machine]. */
@Serializable
data class NewAgent(
    val harness: String,
    val directory: String,
    val category: String,
    val machine: String? = null,
    val model: String? = null,
    val mode: String? = null,
)

/** An ssh host the Mac can start agents on, most used first. */
@Serializable
data class Machine(
    val name: String,
    val uses: Int,
    val available: Boolean,
)

@Serializable
data class FrequentFolder(
    val path: String,
    val count: Int,
)

/** A machine's folders; [unchanged] when the phone already holds [version]. */
@Serializable
data class FolderPayload(
    val version: String,
    val unchanged: Boolean? = null,
    val home: String? = null,
    val folders: List<String>? = null,
    val frequent: List<FrequentFolder>? = null,
    val harnesses: Map<String, String>? = null,
)

@Serializable
data class StartedAgent(
    val id: String,
    /** The CLI updates itself before the agent starts. */
    val updating: Boolean,
)

@Serializable
data class ScreenFrame(
    val revision: Long? = null,
    val columns: Int,
    val rows: Int,
    val cursor: Cursor,
    val alternateScreen: Boolean,
    val applicationCursor: Boolean,
    val foreground: String,
    val background: String,
    val lines: List<List<Run>>,
) {
    @Serializable
    data class Cursor(
        val x: Int,
        val y: Int,
        val visible: Boolean,
    )

    val text: String
        get() = lines.joinToString("\n") { line -> line.joinToString("") { it.text } }
}

/**
 * One styled stretch of a row, decoded from the gateway's positional array
 * `[text, foreground, background, flags, column, width]` where the trailing
 * `column`/`width` are absent from older gateways. The custom serializer is
 * the port of `Run.init(from:)` in Gateway.swift.
 */
@Serializable(with = RunSerializer::class)
data class Run(
    val text: String,
    val foreground: String?,
    val background: String?,
    val flags: Int,
    /** Starting column and width in cells; absent from older gateways. */
    val column: Int?,
    val width: Int?,
) {
    companion object {
        const val BOLD = 1
        const val ITALIC = 2
        const val FAINT = 4
        const val UNDERLINE = 8
        const val STRIKE = 16
        const val CURSOR = 32
    }
}

object RunSerializer : kotlinx.serialization.KSerializer<Run> {
    override val descriptor =
        kotlinx.serialization.json.JsonArray.serializer().descriptor

    private fun JsonElement?.optionalString(): String? =
        this?.takeIf { it !is JsonNull }?.jsonPrimitive?.content

    private fun JsonElement?.optionalInt(): Int? =
        this?.takeIf { it !is JsonNull }?.jsonPrimitive?.content?.toIntOrNull()

    override fun deserialize(decoder: kotlinx.serialization.encoding.Decoder): Run {
        val values = JsonArray.serializer().deserialize(decoder)
        fun at(index: Int): JsonElement? = values.getOrNull(index)
        return Run(
            text = at(0).optionalString()
                ?: throw IllegalArgumentException("run: missing text"),
            foreground = at(1).optionalString(),
            background = at(2).optionalString(),
            flags = at(3).optionalInt()
                ?: throw IllegalArgumentException("run: missing flags"),
            column = at(4).optionalInt(),
            width = at(5).optionalInt(),
        )
    }

    override fun serialize(
        encoder: kotlinx.serialization.encoding.Encoder,
        value: Run,
    ) {
        val elements = buildList {
            add(JsonPrimitive(value.text))
            value.foreground?.let { add(JsonPrimitive(it)) } ?: add(JsonNull)
            value.background?.let { add(JsonPrimitive(it)) } ?: add(JsonNull)
            add(JsonPrimitive(value.flags))
            if (value.column != null || value.width != null) {
                value.column?.let { add(JsonPrimitive(it)) }
                value.width?.let { add(JsonPrimitive(it)) }
            }
        }
        JsonArray.serializer().serialize(encoder, JsonArray(elements))
    }
}

/** An archived page of output above the live screen, oldest first on screen. */
@Serializable
data class HistoryPage(
    val page: Long,
    val message: String,
    val end: Boolean,
    val busy: Boolean,
    val columns: Int? = null,
    val lines: List<List<Run>>? = null,
)

@Serializable
data class StreamStatus(
    val state: String,
    val message: String,
)

@Serializable
data class Attached(
    /** False when the agent's service predates joining, so the phone took the agent from the Mac instead of showing it alongside. */
    val shared: Boolean,
)

/** Events of `api/agents/{id}/stream`; each `data:` line is one complete JSON event. */
sealed class StreamEvent {
    /** The fully qualified payload type: `Attached` would resolve to this nested class itself. */
    data class Attached(val attached: dev.lapis.remote.gateway.Attached) : StreamEvent()

    /** The decoded screen and the exact JSON it came from (sent with captures). */
    data class Frame(val frame: ScreenFrame, val json: String) : StreamEvent()

    data class Status(val status: StreamStatus) : StreamEvent()
}

/** The gateway's named navigation keys; letters are not in the key table. */
enum class Key(val wire: String) {
    UP("up"),
    DOWN("down"),
    LEFT("left"),
    RIGHT("right"),
    HOME("home"),
    END("end"),
    PAGE_UP("pageUp"),
    PAGE_DOWN("pageDown"),
    DELETE("delete"),
    ENTER("enter"),
    TAB("tab"),
    BACKSPACE("backspace"),
    ESCAPE("escape"),
}

/** One input POST body. TEXT passes through verbatim; only `paste` is re-encoded. */
@Serializable
data class Input(
    val text: String? = null,
    val paste: String? = null,
    val key: String? = null,
    val modifiers: Int? = null,
    val resize: List<Int>? = null,
) {
    companion object {
        /** Modifier nibble on `key`: shift = 1, control = 2, alt = 4. */
        fun key(key: Key, shift: Boolean = false): Input =
            Input(key = key.wire, modifiers = if (shift) 1 else 0)
    }
}

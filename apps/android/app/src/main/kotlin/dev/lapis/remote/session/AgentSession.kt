package dev.lapis.remote.session

import dev.lapis.remote.gateway.Agent
import dev.lapis.remote.gateway.Attached
import dev.lapis.remote.gateway.GatewayError
import dev.lapis.remote.gateway.Input
import dev.lapis.remote.gateway.Key
import dev.lapis.remote.gateway.LapisGateway
import dev.lapis.remote.gateway.ScreenFrame
import dev.lapis.remote.gateway.StreamEvent
import dev.lapis.remote.gateway.StreamStatus
import dev.lapis.remote.platform.describe
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.takeWhile
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * One agent shown on the phone: the live stream, the input queue, and the
 * archived history above the screen. Port of `AgentSession` in Models.swift.
 *
 * Showing it joins the agent's session beside the Mac, so both stay in sync.
 * All state lives on [scope]'s dispatcher (main in the app, the test
 * scheduler in tests): every mutation happens between suspensions of jobs
 * launched here, and the gateway hops its own I/O to Dispatchers.IO
 * internally, mirroring the @MainActor confinement of the Swift original.
 */
class AgentSession(
    agent: Agent,
    private val gateway: LapisGateway?,
    private val scope: CoroutineScope,
) {
    /** States of the attachment; `Closed.reason` carries the reopen flag. */
    sealed class State {
        data object Connecting : State()

        data object Live : State()

        data class Closed(val reason: String, val reopen: Boolean) : State()
    }

    private val _state = MutableStateFlow<State>(State.Connecting)
    val state: StateFlow<State> = _state

    private val _frame = MutableStateFlow<ScreenFrame?>(null)
    val frame: StateFlow<ScreenFrame?> = _frame

    private val _notice = MutableStateFlow<String?>(null)
    val notice: StateFlow<String?> = _notice

    private val _shared = MutableStateFlow(true)
    val shared: StateFlow<Boolean> = _shared

    /** Archived output above the live screen, oldest first and contiguous with it. */
    private val _history = MutableStateFlow<List<HistoryChunk>>(emptyList())
    val history: StateFlow<List<HistoryChunk>> = _history

    /** The oldest page is loaded. Nothing archived yet is not the start: an empty history is retried. */
    private val _historyEnd = MutableStateFlow(false)
    val historyEnd: StateFlow<Boolean> = _historyEnd

    /** Every kept row, and whether the service can jump to any of them. */
    private val _historyTotal = MutableStateFlow(0)
    val historyTotal: StateFlow<Int> = _historyTotal

    private val _scrubbable = MutableStateFlow(false)
    val scrubbable: StateFlow<Boolean> = _scrubbable

    /** After a jump: pages are skipped between the shown ones and the live screen, and the page jumped to. */
    private val _gapAfter = MutableStateFlow(false)
    val gapAfter: StateFlow<Boolean> = _gapAfter

    private val _jumpedTo = MutableStateFlow<UUID?>(null)
    val jumpedTo: StateFlow<UUID?> = _jumpedTo

    private val _loadingHistory = MutableStateFlow(false)
    val loadingHistory: StateFlow<Boolean> = _loadingHistory

    private val _inputPending = MutableStateFlow(false)
    val inputPending: StateFlow<Boolean> = _inputPending

    /** The exact JSON of the newest frame (sent with captures). */
    private val _lastFrameJson = MutableStateFlow<String?>(null)
    val lastFrameJson: StateFlow<String?> = _lastFrameJson

    private val _size = MutableStateFlow<Grid?>(null)
    val size: StateFlow<Grid?> = _size

    /** The Mac may rename the agent mid-session; the id (the identity) never changes. */
    var agent: Agent = agent
        private set

    data class Grid(val columns: Int, val rows: Int)

    private var pendingJumpFraction: Double? = null
    private var lastEmptyCheckNanos = Long.MIN_VALUE
    private var newerJob: Job? = null
    private var historyJob: Job? = null
    private var historyPrefetched = false

    /** One identity for the attachment that owns stream, input and history work. */
    private var connectionGeneration = UUID.randomUUID()
    private var streamJob: Job? = null
    private var inputJob: Job? = null
    private val inputQueue = ArrayDeque<Input>()

    init {
        // The last screen seen shows at once while the live one connects.
        _frame.value = ScreenCache.frame(agent.id)
    }

    val isLive: Boolean get() = _state.value == State.Live

    fun open(columns: Int, rows: Int) {
        if (gateway == null) {
            _state.value = State.Closed(describe(GatewayError.InvalidHost), reopen = false)
            return
        }
        close()
        _size.value = Grid(columns, rows)
        val generation = connectionGeneration
        _state.value = State.Connecting
        _history.value = emptyList()
        _historyEnd.value = false
        _gapAfter.value = false
        _jumpedTo.value = null
        pendingJumpFraction = null
        lastEmptyCheckNanos = Long.MIN_VALUE
        streamJob = scope.launch {
            try {
                gateway.stream(agent.id, columns, rows)
                    // An old attachment must not publish over a newer one.
                    .takeWhile { connectionGeneration == generation && isActive }
                    .collect { event -> handle(event, columns, rows, generation) }
                // A normal end with the generation intact is the gateway
                // closing the connection; a retired generation stays silent.
                if (connectionGeneration == generation) {
                    closedByGateway()
                    retireConnection()
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                if (connectionGeneration == generation) {
                    _state.value = State.Closed(describe(failure), reopen = true)
                    retireConnection()
                }
            }
        }
    }

    private fun handle(event: StreamEvent, columns: Int, rows: Int, generation: UUID) {
        when (event) {
            is StreamEvent.Attached -> _shared.value = event.attached.shared
            is StreamEvent.Frame -> {
                val firstFrame = _state.value == State.Connecting
                _frame.value = event.frame
                _lastFrameJson.value = event.json
                _state.value = State.Live
                // The stream carries the opening size; send any resize made
                // during connection once the first frame arrives.
                val size = _size.value
                if (firstFrame && size != null && (size.columns != columns || size.rows != rows)) {
                    send(Input(resize = listOf(size.columns, size.rows)))
                }
                ScreenCache.store(agent.id, event.frame)
                followNewHistory(generation)
                // The newest archived page, before the first scroll up asks.
                if (!historyPrefetched) {
                    historyPrefetched = true
                    historyJob = scope.launch { loadOlder(generation) }
                }
            }
            is StreamEvent.Status -> {
                _state.value = State.Closed(explain(event.status), event.status.state == "disconnected")
                retireConnection()
                streamJob?.cancel()
            }
        }
    }

    private fun closedByGateway() {
        if (_state.value == State.Live || _state.value == State.Connecting) {
            _state.value = State.Closed("The Mac closed the connection.", reopen = true)
        }
    }

    fun close() {
        streamJob?.cancel()
        streamJob = null
        retireConnection()
    }

    private fun retireConnection() {
        connectionGeneration = UUID.randomUUID()
        retireHistory()
        inputJob?.cancel()
        inputJob = null
        inputQueue.clear()
        _inputPending.value = false
    }

    /** A status or transport end retires work owned by that attachment. */
    private fun retireHistory() {
        historyJob?.cancel()
        historyJob = null
        newerJob?.cancel()
        newerJob = null
        historyPrefetched = false
        _loadingHistory.value = false
    }

    fun updateAgent(refreshed: Agent) {
        if (refreshed.id == agent.id) agent = refreshed
    }

    fun clearNotice() {
        _notice.value = null
    }

    fun resize(columns: Int, rows: Int) {
        val size = _size.value
        if (size?.columns == columns && size?.rows == rows) return
        _size.value = Grid(columns, rows)
        if (isLive) send(Input(resize = listOf(columns, rows)))
    }

    fun send(input: Input) {
        if (gateway == null || !isLive) return
        if (inputQueue.size >= 128) {
            _notice.value = "Input is still being sent. Wait for the Mac before typing more."
            return
        }
        inputQueue.addLast(input)
        if (inputJob != null) return
        val id = agent.id
        val generation = connectionGeneration
        _inputPending.value = true
        inputJob = scope.launch {
            try {
                while (isActive && connectionGeneration == generation && inputQueue.isNotEmpty()) {
                    val next = inputQueue.first()
                    try {
                        // A compound paste and Enter completes before the next key.
                        // The head leaves the queue only once its POST is
                        // answered, so the 128 cap bounds outstanding input
                        // (in flight included), as on iOS.
                        gateway!!.send(next, id)
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        if (connectionGeneration == generation) {
                            inputQueue.clear()
                            _notice.value = describe(failure)
                        }
                        return@launch
                    }
                    inputQueue.removeFirst()
                }
            } finally {
                if (connectionGeneration == generation) {
                    inputJob = null
                    _inputPending.value = false
                }
            }
        }
    }

    /** Paste the message and press Enter, as typing it would. */
    fun submit(message: String) {
        if (message.isEmpty()) {
            send(Input.key(Key.ENTER))
        } else {
            send(Input(paste = message, key = Key.ENTER.wire))
        }
    }

    /** Loads the page before the oldest one shown; the gateway archives what scrolled off the top of the agent's terminal. */
    suspend fun loadOlder() {
        loadOlder(connectionGeneration)
    }

    private suspend fun loadOlder(generation: UUID) {
        if (connectionGeneration != generation || gateway == null || !isLive ||
            _historyEnd.value || _loadingHistory.value
        ) {
            return
        }
        _loadingHistory.value = true
        try {
            performLoadOlder(generation)
            if (connectionGeneration != generation) return
            runPendingJump(generation)
        } finally {
            // A replacement already cleared this flag for the new attachment.
            if (connectionGeneration == generation) _loadingHistory.value = false
        }
    }

    /** While history is shown, pages archived since it loaded are appended so it stays contiguous with the live screen (after a jump, only on asking). */
    private fun followNewHistory(generation: UUID) {
        if (connectionGeneration != generation || _history.value.isEmpty() || _gapAfter.value ||
            newerJob != null
        ) {
            return
        }
        newerJob = scope.launch {
            delay(1_000)
            if (connectionGeneration != generation || !currentCoroutineContext().isActive) return@launch
            loadNewer(generation)
            if (connectionGeneration != generation) return@launch
            newerJob = null
        }
    }

    private suspend fun loadNewer(generation: UUID) {
        // A jump can land between scheduling this delayed load and its first
        // await. Intentionally skipped pages stay skipped until closeGap.
        val newest = _history.value.lastOrNull()?.page ?: return
        if (connectionGeneration != generation || gateway == null || !isLive ||
            _loadingHistory.value || _gapAfter.value
        ) {
            return
        }
        _loadingHistory.value = true
        try {
            performLoadNewer(newest, generation)
            if (connectionGeneration != generation) return
            runPendingJump(generation)
        } finally {
            if (connectionGeneration == generation) _loadingHistory.value = false
        }
    }

    /** Every kept row, as the last page said; the scrubber's scale. */
    private fun note(place: dev.lapis.remote.gateway.HistoryPage.Place?) {
        if (place == null) return
        _historyTotal.value = place.total
        _scrubbable.value = _scrubbable.value || place.scrubbable
    }

    /** Shows the page at [fraction] of everything kept (0 the oldest row), alone: older pages load above it as before, and the newer ones between it and the live screen when asked (closeGap). */
    suspend fun jump(fraction: Double) {
        val generation = connectionGeneration
        if (gateway == null || connectionGeneration != generation || !isLive ||
            !_scrubbable.value || _historyTotal.value <= 0
        ) {
            return
        }
        if (_loadingHistory.value) {
            pendingJumpFraction = fraction
            return
        }
        _loadingHistory.value = true
        try {
            performJump(fraction, generation)
        } finally {
            if (connectionGeneration == generation) _loadingHistory.value = false
        }
        if (connectionGeneration != generation) return
        runPendingJump(generation)
    }

    /** Loads the pages skipped between a jumped-to page and the live screen, a screenful of them at a time. */
    suspend fun closeGap() {
        val generation = connectionGeneration
        val newest = _history.value.lastOrNull()?.page
        if (gateway == null || connectionGeneration != generation || !isLive ||
            !_gapAfter.value || newest == null || _loadingHistory.value
        ) {
            return
        }
        _loadingHistory.value = true
        try {
            performCloseGap(newest, generation)
        } finally {
            if (connectionGeneration == generation) _loadingHistory.value = false
        }
        if (connectionGeneration != generation) return
        runPendingJump(generation)
    }

    private suspend fun runPendingJump(generation: UUID) {
        val fraction = pendingJumpFraction ?: return
        if (connectionGeneration != generation) return
        pendingJumpFraction = null
        jump(fraction)
    }

    private suspend fun performLoadOlder(generation: UUID) {
        val gateway = gateway ?: return
        // While nothing is archived, ask at most once a second, but always
        // ask again after the latest output (callers repeat on new output).
        if (_history.value.isEmpty()) {
            val now = System.nanoTime()
            val since = (now - lastEmptyCheckNanos) / 1_000_000_000.0
            if (lastEmptyCheckNanos != Long.MIN_VALUE && since < 1.0) {
                delay(((1.0 - since) * 1000).toLong())
                if (connectionGeneration != generation || !currentCoroutineContext().isActive) return
            }
        }
        // Pages can hold only a few rows; gather more than a screen per load
        // so the loaded rows move the top out of view until the next scroll.
        var before = _history.value.firstOrNull()?.page ?: 0L
        var gathered = 0
        var attempts = 0
        while (gathered < 80 && attempts < 40) {
            if (connectionGeneration != generation || !currentCoroutineContext().isActive) return
            attempts += 1
            val reply = runCatching { gateway.history(agent.id, before = before) }.getOrNull() ?: return
            if (connectionGeneration != generation || !currentCoroutineContext().isActive) return
            if (reply.busy) {
                delay(300)
                continue
            }
            val lines = reply.lines
            val columns = reply.columns
            if (lines != null && columns != null && reply.page != 0L) {
                _history.value = listOf(
                    HistoryChunk(page = reply.page, columns = columns, lines = lines, offset = reply.place?.offset),
                ) + _history.value
                note(reply.place)
                gathered += lines.size
                before = reply.page
                continue
            }
            if (connectionGeneration != generation) return
            if (_history.value.isEmpty()) {
                lastEmptyCheckNanos = System.nanoTime()
            } else {
                _historyEnd.value = true
            }
            return
        }
    }

    private suspend fun performLoadNewer(after: Long, generation: UUID) {
        val gateway = gateway ?: return
        var cursor = after
        repeat(20) {
            if (connectionGeneration != generation || !currentCoroutineContext().isActive) return
            val reply = runCatching { gateway.history(agent.id, after = cursor) }.getOrNull() ?: return
            if (reply.busy || reply.page == 0L) return
            val lines = reply.lines ?: return
            val columns = reply.columns ?: return
            if (connectionGeneration != generation || !currentCoroutineContext().isActive) return
            _history.value = _history.value + HistoryChunk(
                page = reply.page, columns = columns, lines = lines, offset = reply.place?.offset,
            )
            note(reply.place)
            cursor = reply.page
        }
    }

    private suspend fun performJump(fraction: Double, generation: UUID) {
        val gateway = gateway ?: return
        val total = _historyTotal.value
        val row = (fraction.coerceIn(0.0, 1.0) * (total - 1).toDouble()).let { Math.round(it).toInt() }
        val reply = runCatching { gateway.history(agent.id, at = row) }.getOrNull() ?: return
        val lines = reply.lines ?: return
        val columns = reply.columns ?: return
        if (reply.page == 0L || connectionGeneration != generation || !currentCoroutineContext().isActive) return
        note(reply.place)
        val chunk = HistoryChunk(
            page = reply.page, columns = columns, lines = lines, offset = reply.place?.offset,
        )
        _history.value = listOf(chunk)
        val place = reply.place
        _historyEnd.value = place?.offset == 0
        _gapAfter.value = place?.let { it.offset + it.rows < it.total } ?: false
        _jumpedTo.value = chunk.cacheId
    }

    private suspend fun performCloseGap(after: Long, generation: UUID) {
        val gateway = gateway ?: return
        var cursor = after
        repeat(8) {
            if (connectionGeneration != generation || !currentCoroutineContext().isActive) return
            val reply = runCatching { gateway.history(agent.id, after = cursor) }.getOrNull() ?: return
            if (reply.busy) return
            if (connectionGeneration != generation || !currentCoroutineContext().isActive) return
            val lines = reply.lines
            val columns = reply.columns
            if (reply.page == 0L || lines == null || columns == null) {
                _gapAfter.value = false
                return
            }
            _history.value = _history.value + HistoryChunk(
                page = reply.page, columns = columns, lines = lines, offset = reply.place?.offset,
            )
            note(reply.place)
            cursor = reply.page
        }
    }

    companion object {
        fun explain(status: StreamStatus): String = when (status.state) {
            "replaced" -> "The Mac took this agent back. Open it here again to continue."
            "ended" -> "This agent has ended."
            "released" -> "This agent was opened in another view."
            "overloaded" -> "The agent's session was overloaded and closed the view."
            else -> status.message.ifEmpty { "The agent's session closed." }
        }
    }
}

/** One loaded page of archived output. Page numbers can be reused by a restarted service; cache by [cacheId]. */
data class HistoryChunk(
    val cacheId: UUID = UUID.randomUUID(),
    val page: Long,
    val columns: Int,
    val lines: List<List<dev.lapis.remote.gateway.Run>>,
    /** Its first row among every kept row, when the service says. */
    val offset: Int? = null,
)

/** The last screen seen of each agent, shown at once when it is opened again while the live screen connects. */
object ScreenCache {
    private var frames: Map<String, Pair<ScreenFrame, Long>> = emptyMap()

    fun frame(agent: String): ScreenFrame? = frames[agent]?.first

    fun age(agent: String, now: Long = System.nanoTime()): Double =
        frames[agent]?.let { (now - it.second) / 1_000_000_000.0 } ?: Double.POSITIVE_INFINITY

    fun store(agent: String, frame: ScreenFrame) {
        frames = frames + (agent to (frame to System.nanoTime()))
    }

    fun clearAll() {
        frames = emptyMap()
    }
}

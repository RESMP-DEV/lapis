package dev.lapis.remote.session

import dev.lapis.remote.gateway.GatewayError
import dev.lapis.remote.gateway.GatewayTransport
import dev.lapis.remote.gateway.LapisGateway
import dev.lapis.remote.gateway.OkHttpTransport
import dev.lapis.remote.gateway.WorkspaceListing
import dev.lapis.remote.gateway.gatewayJson
import dev.lapis.remote.platform.describe
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Builds the [LapisGateway] for the configured host, or returns null when the
 * host does not normalize (the caller shows the invalid-host notice). The seam
 * exists so tests can hand the repository a gateway over a fake transport.
 */
fun interface GatewaySource {
    fun gateway(host: String): LapisGateway?
}

class StandardGatewaySource(
    private val transport: GatewayTransport = OkHttpTransport(),
    private val ioContext: kotlin.coroutines.CoroutineContext = Dispatchers.IO,
) : GatewaySource {
    override fun gateway(host: String): LapisGateway? = try {
        LapisGateway.build(host, transport, ioContext)
    } catch (_: GatewayError) {
        null
    }
}

/**
 * The workspace: the configured gateway host, the agent listing, and the
 * per-host disk cache. Port of the milestone-A surface of `WorkspaceModel`
 * (Models.swift): any host change invalidates in-flight work via a generation
 * counter and restores the new host's disk cache, so a stale answer can never
 * publish over a newer host. Parsing stays off the caller's (main) thread.
 *
 * Agent sessions arrive in milestone B; this class stays listing-scoped.
 */
class WorkspaceRepository(
    private val sources: GatewaySource,
    private val store: dev.lapis.remote.platform.KeyValueStore,
    private val cache: DiskCache?,
    private val scope: CoroutineScope,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    companion object {
        /** DataStore key for the gateway host, matching iOS "gatewayHost". */
        const val HOST_KEY = "gatewayHost"

        fun listingCacheKey(host: String): String =
            "${DiskCache.sha256Hex(host.lowercase())}-listing"
    }

    private val generation = java.util.concurrent.atomic.AtomicLong(0)

    private val _host = MutableStateFlow("")
    val host: StateFlow<String> = _host

    private val _listing = MutableStateFlow<WorkspaceListing?>(null)
    val listing: StateFlow<WorkspaceListing?> = _listing

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _refreshing = MutableStateFlow(false)
    val refreshing: StateFlow<Boolean> = _refreshing

    /** A gateway for [host], for surfaces beyond the listing (the stage's session); null when the host does not normalize. */
    fun gatewayFor(host: String): LapisGateway? = sources.gateway(host)

    /** Restores the persisted host (and through it, the cached listing) at launch. */
    suspend fun restore() {
        // An explicitly set host (a debug launch override) wins over the
        // persisted one; its async save may not have landed yet.
        if (_host.value.isNotEmpty()) return
        val saved = store.getString(HOST_KEY) ?: return
        setHost(saved)
    }

    /**
     * Switches the gateway host. Invalidation is synchronous: the generation
     * moves immediately, so any refresh started earlier becomes stale; the
     * persisted write and the new host's cache restore run on [scope].
     */
    fun setHost(raw: String) {
        val value = raw.trim()
        if (value == _host.value) return
        val current = generation.incrementAndGet()
        _host.value = value
        _error.value = null
        _refreshing.value = false
        _listing.value = null
        scope.launch { store.putString(HOST_KEY, value) }
        scope.launch(io) {
            val cached = decodeCachedListing(value)
            if (generation.get() == current) {
                // The last known state of this Mac shows at once; it is
                // refreshed right after. Only an empty slot takes the cache,
                // so a network answer that already landed stays on top.
                _listing.compareAndSet(null, cached)
            }
        }
    }

    private fun decodeCachedListing(host: String): WorkspaceListing? {
        val cached = cache?.load(listingCacheKey(host)) ?: return null
        return runCatching {
            gatewayJson.decodeFromString(WorkspaceListing.serializer(), cached)
        }.getOrNull()
    }

    /**
     * Fetches the listing from the gateway. When the host changed while the
     * request was in flight, the answer is dropped: it belongs to an older
     * generation. Failure keeps any listing already on screen and reports the
     * error beside it, as iOS does.
     */
    suspend fun refresh() {
        val host = _host.value
        if (host.isEmpty()) {
            _error.value = describe(GatewayError.InvalidHost)
            return
        }
        val gateway = sources.gateway(host)
        if (gateway == null) {
            _error.value = describe(GatewayError.InvalidHost)
            return
        }
        val current = generation.get()
        _refreshing.value = true
        try {
            val outcome = withContext(io) {
                // Cancellation must propagate, not surface as an error the
                // way runCatching would (iOS ignores CancellationError too).
                try {
                    Result.success(gateway.agents())
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Throwable) {
                    Result.failure(failure)
                }
            }
            if (generation.get() != current) return
            outcome
                .onSuccess { listing ->
                    _listing.value = listing
                    _error.value = null
                    cache?.let { disk ->
                        scope.launch(io) {
                            runCatching {
                                disk.save(
                                    listingCacheKey(host),
                                    gatewayJson.encodeToString(
                                        WorkspaceListing.serializer(),
                                        listing,
                                    ),
                                )
                            }
                        }
                    }
                }
                .onFailure { failure -> _error.value = describe(failure) }
        } finally {
            if (generation.get() == current) _refreshing.value = false
        }
    }
}

/** Application-scope holder so the repository outlives activity recreation. */
object RepositoryFactory {
    fun create(
        store: dev.lapis.remote.platform.KeyValueStore,
        cache: DiskCache?,
    ): WorkspaceRepository = WorkspaceRepository(
        sources = StandardGatewaySource(),
        store = store,
        cache = cache,
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
    )
}

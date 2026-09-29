package dev.lapis.remote.session

import dev.lapis.remote.gateway.GatewayRequest
import dev.lapis.remote.gateway.GatewayResponse
import dev.lapis.remote.gateway.GatewayStream
import dev.lapis.remote.gateway.GatewayTransport
import dev.lapis.remote.gateway.LapisGateway
import dev.lapis.remote.platform.KeyValueStore
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.fail
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest

/** Deterministic transport: records requests and replays canned answers. */
private class FakeTransport(
    var handler: (GatewayRequest) -> GatewayResponse = {
        fail("unexpected request ${it.url}")
    },
) : GatewayTransport {
    val requests = mutableListOf<GatewayRequest>()
    override fun call(request: GatewayRequest): GatewayResponse {
        requests += request
        return handler(request)
    }

    override fun open(request: GatewayRequest): GatewayStream =
        fail("streams are not part of the milestone-A repository surface")
}

private class MemoryStore : KeyValueStore {
    val values = mutableMapOf<String, String>()
    override suspend fun getString(key: String): String? = values[key]
    override suspend fun putString(key: String, value: String) {
        values[key] = value
    }
}

private fun fixture(name: String): String =
    requireNotNull(WorkspaceRepositoryTest::class.java.classLoader)
        .getResourceAsStream("fixtures/$name")!!
        .readBytes().toString(Charsets.UTF_8)

/**
 * Generation-guard behavior ported from WorkspaceModel (Models.swift): a
 * response that lands after a host change belongs to an older generation and
 * must never publish, and the disk cache follows the host.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class WorkspaceRepositoryTest {
    private val cacheDir = File(
        File(requireNotNull(System.getProperty("java.io.tmpdir"))),
        "lapis-cache-test-${System.nanoTime()}",
    )

    @AfterTest
    fun cleanUp() {
        cacheDir.deleteRecursively()
    }

    private fun TestScope.repository(
        dispatcher: kotlinx.coroutines.CoroutineDispatcher,
        transport: GatewayTransport,
        store: KeyValueStore = MemoryStore(),
        cache: DiskCache? = DiskCache(cacheDir),
    ): WorkspaceRepository = WorkspaceRepository(
        sources = { host ->
            try {
                LapisGateway.build(host, transport, dispatcher)
            } catch (_: Exception) {
                null
            }
        },
        store = store,
        cache = cache,
        // The test scope itself, so the repository's own launches (cache
        // restore, persistence) are children that advanceUntilIdle pumps.
        scope = this,
        io = dispatcher,
    )

    @Test
    fun staleResponseAfterHostChangeNeverPublishes() {
        runTest(StandardTestDispatcher()) {
            // The mac-a answer is parked inside the transport on a real IO
            // thread, so the host change can land while it is in flight.
            val arrived = CountDownLatch(1)
            val release = CountDownLatch(1)
            val transport = FakeTransport(
                handler = { _ ->
                    arrived.countDown()
                    assertTrue(release.await(10, TimeUnit.SECONDS), "the parked answer was never released")
                    GatewayResponse(200, fixture("workspace_listing.json").encodeToByteArray())
                },
            )
            val repo = repository(Dispatchers.IO, transport)

            repo.setHost("mac-a")
            advanceUntilIdle()
            assertNull(repo.listing.value)

            val refresh = launch { repo.refresh() }
            advanceUntilIdle() // let the refresh reach the transport
            assertTrue(arrived.await(10, TimeUnit.SECONDS), "the refresh never reached the transport")
            assertTrue(transport.requests.single().url.startsWith("http://mac-a:"))

            // The user switches Macs while the answer is in flight.
            repo.setHost("mac-b")
            release.countDown()
            refresh.join()

            assertNull(repo.listing.value, "the mac-a answer belongs to an older generation")
            assertNull(repo.error.value)

            // A fresh refresh against the current host publishes normally.
            transport.handler = {
                GatewayResponse(200, fixture("workspace_listing_legacy.json").encodeToByteArray())
            }
            repo.refresh()
            assertEquals("cat-only", repo.listing.value?.activeCategory)
        }
    }

    @Test
    fun hostChangeRestoresTheNewHostsDiskCache() = runTest(StandardTestDispatcher()) {
        val transport = FakeTransport(
            handler = { GatewayResponse(200, fixture("workspace_listing.json").encodeToByteArray()) },
        )
        val repo = repository(StandardTestDispatcher(testScheduler), transport)

        // Populate mac-a's cache with a successful refresh.
        repo.setHost("mac-a")
        advanceUntilIdle()
        repo.refresh()
        advanceUntilIdle()
        assertEquals("cat-work", repo.listing.value?.activeCategory)
        assertTrue(
            File(cacheDir, "${WorkspaceRepository.listingCacheKey("mac-a")}.json").isFile,
            "the listing lands in the per-host cache file",
        )

        // Switch to mac-b: no cache exists, so the mac-a listing must go, and
        // the network is not consulted for the swap itself.
        transport.handler = { fail("the network must not be needed to show a cached listing") }
        repo.setHost("mac-b")
        advanceUntilIdle()
        assertNull(repo.listing.value, "a new host starts from its own (empty) cache")

        // Switch back: mac-a's cached listing shows before any network answer.
        repo.setHost("mac-a")
        advanceUntilIdle()
        assertEquals("cat-work", repo.listing.value?.activeCategory)
        assertEquals(1, transport.requests.size, "no additional request was needed")
    }

    @Test
    fun restoreReadsThePersistedHostAndItsCache() = runTest(StandardTestDispatcher()) {
        val store = MemoryStore()
        store.values["gatewayHost"] = "mac-a"
        val cache = DiskCache(cacheDir)
        cache.save(WorkspaceRepository.listingCacheKey("mac-a"), fixture("workspace_listing_legacy.json"))
        val repo = repository(
            StandardTestDispatcher(testScheduler),
            FakeTransport(),
            store,
            cache,
        )

        repo.restore()
        advanceUntilIdle()
        assertEquals("mac-a", repo.host.value)
        assertEquals("cat-only", repo.listing.value?.activeCategory, "cold launch shows the last listing at once")
    }

    @Test
    fun invalidHostReportsInsteadOfPublishing() = runTest(StandardTestDispatcher()) {
        val repo = repository(StandardTestDispatcher(testScheduler), FakeTransport())

        repo.setHost("")
        repo.refresh()
        assertEquals("Set the Mac's Tailscale name or ZeroTier address in Settings.", repo.error.value)
        assertNull(repo.listing.value)

        repo.setHost("ftp://mac")
        repo.refresh()
        assertEquals("Set the Mac's Tailscale name or ZeroTier address in Settings.", repo.error.value)
        assertEquals("ftp://mac", repo.host.value, "the invalid text is kept; the gateway simply fails to build")
    }

    @Test
    fun failedRefreshKeepsTheListingAndReportsTheError() = runTest(StandardTestDispatcher()) {
        val transport = FakeTransport(
            handler = { GatewayResponse(200, fixture("workspace_listing_legacy.json").encodeToByteArray()) },
        )
        val repo = repository(StandardTestDispatcher(testScheduler), transport)
        repo.setHost("mac-a")
        repo.refresh()
        advanceUntilIdle()
        assertEquals("cat-only", repo.listing.value?.activeCategory)

        transport.handler = {
            GatewayResponse(503, """{"error": "overloaded"}""".encodeToByteArray())
        }
        repo.refresh()
        advanceUntilIdle()
        assertEquals("overloaded", repo.error.value)
        assertEquals("cat-only", repo.listing.value?.activeCategory, "a failed refresh does not blank the last known listing")
        assertEquals(false, repo.refreshing.value)
    }
}

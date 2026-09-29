package dev.lapis.remote.gateway

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

/**
 * The URL normalization table, mirroring Gateway.init(host:) in Gateway.swift:
 * empty host invalid, http:// prefix default, port 7349 default, http/https
 * only, path/query discarded.
 */
class GatewayUrlTest {
    private fun base(host: String): String = LapisGateway.normalizeBase(host)

    @Test
    fun bareHostGetsHttpAndDefaultPort() {
        assertEquals("http://mac.tailnet.ts.net:7349", base("mac.tailnet.ts.net"))
    }

    @Test
    fun hostWithPortKeepsItsPort() {
        assertEquals("http://mac.tailnet.ts.net:7000", base("mac.tailnet.ts.net:7000"))
    }

    @Test
    fun urlInputKeepsSchemeAndDiscardsPath() {
        assertEquals("http://mac:7349", base("http://mac"))
        assertEquals("https://mac.tailnet.ts.net:8443", base("https://mac.tailnet.ts.net:8443/agents"))
        assertEquals("http://mac:7349", base("http://mac/api/agents?x=1"))
        assertEquals("http://mac:7349", base("http://mac/#frag"))
    }

    @Test
    fun explicitDefaultPortIsNotDuplicated() {
        assertEquals("http://mac:7349", base("http://mac:7349"))
    }

    @Test
    fun schemeCaseIsNormalizedLikeURLComponents() {
        // Swift lowercases the scheme before the http/https check, so
        // "HTTPS://" is accepted and normalized, not refused.
        assertEquals("https://mac:7349", base("HTTPS://mac"))
        assertEquals("ftp", assertFailsWith<GatewayError.UnsupportedScheme> {
            base("ftp://mac")
        }.scheme)
    }

    @Test
    fun emptyAndUnparseableHostsAreInvalid() {
        assertFailsWith<GatewayError.InvalidHost> { base("") }
        assertFailsWith<GatewayError.InvalidHost> { base("   \n") }
        assertFailsWith<GatewayError.InvalidHost> { base("http://") }
        assertFailsWith<GatewayError.InvalidHost> { base("http://host with spaces") }
        assertFailsWith<GatewayError.InvalidHost> { base("http://mac:notaport") }
        assertFailsWith<GatewayError.InvalidHost> { base("http://mac:99999") }
    }

    @Test
    fun surroundingWhitespaceIsTrimmed() {
        assertEquals("http://mac.tailnet.ts.net:7349", base("  mac.tailnet.ts.net \n"))
    }

    @Test
    fun bracketedIpv6KeepsBracketsAndPort() {
        assertEquals("http://[fd7a:115c:a1e0::1]:7349", base("[fd7a:115c:a1e0::1]"))
        assertEquals("http://[fd7a:115c:a1e0::1]:8000", base("http://[fd7a:115c:a1e0::1]:8000"))
    }

    @Test
    fun userinfoIsPreservedLikeURLComponents() {
        // Swift keeps the userinfo in the base URL; mirror it.
        assertEquals("http://user@mac:7349", base("user@mac"))
    }

    @Test
    fun lapisGatewayBuildsFromNormalizedHost() {
        val gateway = LapisGateway.build("mac.tailnet.ts.net")
        assertEquals("http://mac.tailnet.ts.net:7349", gateway.base)
    }
}

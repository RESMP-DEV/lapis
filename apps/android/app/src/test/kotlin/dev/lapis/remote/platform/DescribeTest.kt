package dev.lapis.remote.platform

import dev.lapis.remote.gateway.GatewayError
import java.io.IOException
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import kotlin.test.Test
import kotlin.test.assertEquals

/** The `describe` connectivity mapping, ported from Models.swift. */
class DescribeTest {
    @Test
    fun gatewayErrorsCarryTheirOwnCopy() {
        assertEquals("Set the Mac's Tailscale name in Settings.", describe(GatewayError.InvalidHost))
        assertEquals(
            "Unsupported gateway URL scheme: ftp. Use http or https.",
            describe(GatewayError.UnsupportedScheme("ftp")),
        )
        assertEquals("The Mac answered 500.", describe(GatewayError.Refused(500, "")))
        assertEquals("Nope.", describe(GatewayError.Refused(404, "Nope.")))
        assertEquals("The Mac's answer could not be read.", describe(GatewayError.Unreadable))
    }

    @Test
    fun quietMacGetsTheActionableLine() {
        val expected = "The Mac did not answer. Check that Tailscale is on here and the Mac is awake."
        assertEquals(expected, describe(ConnectException("refused")))
        assertEquals(expected, describe(UnknownHostException("no dns")))
        assertEquals(expected, describe(SocketTimeoutException("timed out")))
        assertEquals(expected, describe(IOException("broken pipe")))
    }

    @Test
    fun unexpectedFailuresSurfaceTheirMessage() {
        assertEquals("boom", describe(IllegalStateException("boom")))
    }
}

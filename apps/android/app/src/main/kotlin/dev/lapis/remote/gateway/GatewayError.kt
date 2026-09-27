package dev.lapis.remote.gateway

/**
 * Gateway failures with user-facing copy identical to `GatewayError` in
 * apps/ios/Lapis/Gateway.swift.
 */
sealed class GatewayError(message: String) : Exception(message) {
    /** The host field is empty or does not name a host. */
    data object InvalidHost : GatewayError("Set the Mac's Tailscale name in Settings.")

    /** The URL scheme is not http/https; [scheme] is the original spelling. */
    data class UnsupportedScheme(val scheme: String) :
        GatewayError("Unsupported gateway URL scheme: $scheme. Use http or https.")

    /** A non-200 answer; [bodyMessage] is the gateway's `error` field when present. */
    data class Refused(val code: Int, val bodyMessage: String) :
        GatewayError(if (bodyMessage.isEmpty()) "The Mac answered $code." else bodyMessage)

    /** The answer could not be decoded. */
    data object Unreadable : GatewayError("The Mac's answer could not be read.")
}

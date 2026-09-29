package dev.lapis.remote.session

import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest

/**
 * Small JSON files under `filesDir/cache`, so a cold launch shows the last
 * known listing without waiting for the Mac (port of `DiskCache` in
 * Models.swift). Cache files belong to the Mac they came from: callers key
 * them by [sha256Hex] of the host string, so two gateways never share state.
 *
 * Pure JVM: no Android types, fully unit-testable.
 */
class DiskCache(private val dir: File) {
    init {
        dir.mkdirs()
    }

    fun load(name: String): String? {
        val file = File(dir, "$name.json")
        return if (file.isFile) runCatching { file.readText() }.getOrNull() else null
    }

    /** Atomic write: a crash mid-save leaves the previous file intact. */
    fun save(name: String, content: String) {
        dir.mkdirs()
        val target = File(dir, "$name.json")
        // A unique temporary path: two concurrent saves of one name never
        // interleave writes before the move replaces the target.
        val temporary = File.createTempFile(name, ".tmp", dir)
        try {
            temporary.writeText(content)
            runCatching {
                Files.move(
                    temporary.toPath(),
                    target.toPath(),
                    StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE,
                )
            }.onFailure {
                // Some filesystems refuse ATOMIC_MOVE; a plain move is still atomic
                // enough against readers, and correctness beats the fast path.
                // Both moves failing is a broken cache directory, not a crash for
                // the caller: the old file stays and the next save retries.
                runCatching {
                    Files.move(
                        temporary.toPath(),
                        target.toPath(),
                        StandardCopyOption.REPLACE_EXISTING,
                    )
                }
            }
        } finally {
            // A successful move already consumed the temporary file; a failed
            // save leaves nothing behind to accumulate.
            temporary.delete()
        }
    }

    fun clear() {
        dir.deleteRecursively()
        dir.mkdirs()
    }

    companion object {
        /** The per-host cache key: SHA-256 of the host string, hex-encoded. */
        fun sha256Hex(value: String): String =
            MessageDigest.getInstance("SHA-256")
                .digest(value.encodeToByteArray())
                .joinToString("") { "%02x".format(it) }
    }
}

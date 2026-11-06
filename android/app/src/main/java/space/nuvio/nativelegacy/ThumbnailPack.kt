package space.nuvio.nativelegacy

import java.io.*
import java.security.MessageDigest

/** Local portability proof. A digest detects corruption, NOT an untrusted publisher.
 * No URL, credentials or title is included. The old FNV cache name is deliberately
 * not exported as a global video identity. Remote admission needs a stronger key.
 * NVP1 = magic, payload length (BE long), SHA-256 (32 bytes), original NVM3 bytes.
 */
object ThumbnailPack {
    const val MAX_BYTES = 64L * 1024 * 1024
    data class Info(val slots: Int, val tiles: Int, val durationMs: Int, val bytes: Long, val sha256: String)

    fun inspect(file: File): Info {
        require(file.length() in 16..MAX_BYTES) { "size" }
        val digest = MessageDigest.getInstance("SHA-256")
        var tiles = 0
        var consumed = 16L
        val slots: Int
        val duration: Int
        java.security.DigestInputStream(file.inputStream().buffered(), digest).use { raw ->
            val input = DataInputStream(raw)
            require(input.readInt() == 0x4e564d33) { "format" }
            slots = input.readInt(); duration = input.readInt()
            require(duration in 60_000..86_400_000 && slots == duration / 10_000 + 1) { "timeline" }
            require(input.readInt() == 1) { "incomplete" }
            val seen = BooleanArray(slots)
            val buffer = ByteArray(32 * 1024)
            while (true) {
                // read() distinguishes a clean EOF from a truncated record header.
                val first = input.read()
                if (first < 0) break
                val k = (first shl 24) or (input.readUnsignedByte() shl 16) or
                    (input.readUnsignedByte() shl 8) or input.readUnsignedByte()
                val pts = input.readInt(); val len = input.readInt()
                require(k in 0 until slots && !seen[k] && pts in 0..duration + 60_000 && len in 4..1_048_576) { "record" }
                seen[k] = true
                consumed += 12L + len
                require(consumed <= MAX_BYTES) { "size" }
                var remaining = len
                var previous = -1; var last = -1
                var offset = 0
                while (remaining > 0) {
                    val n = minOf(remaining, buffer.size)
                    input.readFully(buffer, 0, n)
                    if (offset == 0) require(buffer[0] == 255.toByte() && buffer[1] == 216.toByte()) { "jpeg" }
                    previous = if (n > 1) buffer[n - 2].toInt() and 255 else last
                    last = buffer[n - 1].toInt() and 255
                    offset += n
                    remaining -= n
                }
                require(previous == 255 && last == 217) { "jpeg" }
                tiles++
            }
        }
        require(tiles > 0 && consumed == file.length()) { "empty or changed" }
        return Info(slots, tiles, duration, consumed, digest.digest().joinToString("") { "%02x".format(it) })
    }

    fun export(source: File, destination: File): Info {
        destination.parentFile?.mkdirs()
        val snapshot = File.createTempFile("thumb-snapshot-", ".nvm", destination.parentFile)
        val tmp = File.createTempFile("thumb-export-", ".part", destination.parentFile)
        try {
            source.inputStream().use { copyBounded(it, snapshot, source.length()) }
            val info = inspect(snapshot)
            DataOutputStream(tmp.outputStream().buffered()).use { out ->
                out.writeInt(0x4e565031); out.writeLong(info.bytes)
                out.write(hexBytes(info.sha256))
                snapshot.inputStream().use { it.copyTo(out, 32 * 1024) }
            }
            move(tmp, destination)
            return info
        } finally { snapshot.delete(); tmp.delete() }
    }

    /** Destination is supplied by trusted caller; the pack never contains paths. */
    fun import(pack: File, destination: File): Info {
        require(pack.length() in 60..MAX_BYTES + 44) { "size" }
        destination.parentFile?.mkdirs()
        val tmp = File.createTempFile("thumb-import-", ".part", destination.parentFile)
        try {
            val expected = ByteArray(32)
            DataInputStream(pack.inputStream().buffered()).use { input ->
                require(input.readInt() == 0x4e565031) { "format" }
                val length = input.readLong()
                require(length in 16..MAX_BYTES && pack.length() == length + 44) { "length" }
                input.readFully(expected)
                copyBounded(input, tmp, length)
            }
            val info = inspect(tmp)
            require(MessageDigest.isEqual(expected, hexBytes(info.sha256))) { "digest" }
            move(tmp, destination)
            return info
        } finally { tmp.delete() }
    }

    private fun copyBounded(input: InputStream, destination: File, length: Long) {
        require(length in 16..MAX_BYTES) { "size" }
        destination.outputStream().buffered().use { out ->
            val buffer = ByteArray(32 * 1024)
            var remaining = length
            while (remaining > 0) {
                val n = input.read(buffer, 0, minOf(remaining, buffer.size.toLong()).toInt())
                require(n > 0) { "truncated" }
                out.write(buffer, 0, n); remaining -= n
            }
            require(input.read() == -1) { "changed or trailing bytes" }
        }
    }
    private fun move(source: File, destination: File) {
        check(source.renameTo(destination)) { "atomic rename failed" }
    }
    private fun hexBytes(hex: String) = ByteArray(32) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
}

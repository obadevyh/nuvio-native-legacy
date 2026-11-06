package space.nuvio.nativelegacy

import android.util.Log
import java.io.File

// SERVERS THAT REFUSE MULTI-RANGE, REMEMBERED ACROSS SESSIONS (06/11/2026, after Nuvio Arc's MultiRangeSupport).
// Cloudflare R2 answers a multi-range GET with HTTP 400; each new title on it used to spend the first request (and,
// before the per-session set, one per probe round) finding that out again. A refusal is kept per service, keyed by
// the last two labels of the host name ("r2.cloudflarestorage.com" buckets share "cloudflarestorage.com"), and tried
// again after RETRY_MS, so a server that starts serving multipart is picked up. Only refusals are stored: a host that
// serves multipart needs no memory (its first request is the multi-range one anyway).
internal class ThumbnailHostMemory(private val file: File?, private val retryMs: Long = RETRY_MS) {
    private val refusedAt = HashMap<String, Long>()
    private var loaded = false

    @Synchronized fun refuses(host: String?, now: Long): Boolean {
        val k = key(host) ?: return false
        load()
        val at = refusedAt[k] ?: return false
        if (now - at in 0 until retryMs) return true
        refusedAt.remove(k); save()
        return false
    }

    @Synchronized fun recordRefusal(host: String?, now: Long) {
        val k = key(host) ?: return
        load()
        if (refusedAt.put(k, now) == null) Log.i("nuvio-miniatura", "[host-memory] $k refuses multi-range: remembered for ${retryMs / 86_400_000} days")
        save()
    }

    private fun load() {
        if (loaded) return
        loaded = true
        try {
            file?.takeIf { it.isFile }?.readLines()?.forEach { line ->
                val p = line.split('\t')
                if (p.size == 2) p[1].toLongOrNull()?.let { refusedAt[p[0]] = it }
            }
        } catch (_: Exception) { }
    }

    private fun save() {
        val f = file ?: return
        try {
            val tmp = File(f.parentFile, f.name + ".tmp")
            tmp.writeText(refusedAt.entries.joinToString("") { "${it.key}\t${it.value}\n" })
            if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
        } catch (_: Exception) { }
    }

    companion object {
        const val RETRY_MS = 7L * 86_400_000L

        /** The service a host belongs to: its last two labels, or the whole name when it has fewer / is an address. */
        fun key(host: String?): String? {
            val h = host?.trim()?.lowercase()?.trimEnd('.')?.takeIf { it.isNotEmpty() } ?: return null
            if (h.all { it.isDigit() || it == '.' } || ':' in h) return h
            val labels = h.split('.')
            // "x.co.uk" / "x.com.br": a short second-level label under a country code takes three labels
            val n = if (labels.size >= 3 && labels.last().length == 2 && labels[labels.size - 2].length <= 3) 3 else 2
            return if (labels.size <= n) h else labels.takeLast(n).joinToString(".")
        }

        // Created once the app context exists (a memory-only instance before that would never persist).
        @Volatile private var shared: ThumbnailHostMemory? = null
        private fun shared(): ThumbnailHostMemory? = shared ?: synchronized(this) {
            shared ?: NvPlayer.contexto()?.filesDir?.let { ThumbnailHostMemory(File(it, "thumbnail-multirange-refused.txt")) }
                ?.also { shared = it }
        }
        fun refuses(host: String?) = shared()?.refuses(host, System.currentTimeMillis()) == true
        fun recordRefusal(host: String?) { shared()?.recordRefusal(host, System.currentTimeMillis()) }
    }
}

package space.nuvio.nativelegacy

import java.io.IOException
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import android.util.Log

/** Owns progressive-player and thumbnail HTTP exchanges from opening through disconnect.
 * This counts owned exchanges, not hidden idle sockets in Android's HTTP implementation. */
internal object MediaHttpBudget {
    const val LIMIT = 4
    private val permits = Semaphore(LIMIT, true)
    // OBJECT-STORAGE HOSTS (06/11/2026, Cloudflare R2 behind PenguPlay): the 4-connection cap exists for debrid
    // CDNs that limit connections per user (TorBox, 22-23/10). R2 has no such limit, but answers each request only
    // after ~865 ms and serves one range per request; with the player holding all 4 slots for most of the run the
    // thumbnail lanes got 1-2, and Dune 4K took 12.2 min. Hosts that refused multi-range give thumbnails their own
    // EXTRA slots; the player keeps all 4 shared ones. The playback gate and the FAST-pacing check still apply.
    const val EXTRA = 6
    private val extra = Semaphore(EXTRA, true)
    private val extraHosts: MutableSet<String> = ConcurrentHashMap.newKeySet()
    fun allowThumbnailLanes(host: String) {
        if (extraHosts.add(host.lowercase())) Log.i("nuvio-http-budget", "[http-budget] own thumbnail slots for ${host.lowercase()}: +$EXTRA")
    }
    internal fun hasOwnLanes(host: String?) = host != null && host.lowercase() in extraHosts
    private data class Lease(val host: String, val owner: String, val onRelease: ()->Unit, val sem: Semaphore)
    private val leases = ConcurrentHashMap<HttpURLConnection, Lease>()
    private val current = AtomicInteger()
    private val peak = AtomicInteger()
    private val opens = AtomicLong()
    private val waitMs = AtomicLong()
    private val byHost = ConcurrentHashMap<String, AtomicInteger>()
    private val threadWait = object : ThreadLocal<Long>() { override fun initialValue() = 0L }
    fun threadWaitNanos(): Long = threadWait.get()

    fun open(url: URL, owner: String, factory: (URL) -> HttpURLConnection = { it.openConnection() as HttpURLConnection },
             checkCancelled: () -> Unit = {}): HttpURLConnection = openOwned(url,owner,factory,checkCancelled,{})
    fun openOwned(url: URL, owner: String, factory: (URL) -> HttpURLConnection,
             checkCancelled: () -> Unit, onRelease: ()->Unit): HttpURLConnection {
        val start = System.nanoTime()
        val timelinePermit = if (owner.startsWith("thumbnail")) ThumbnailPipelineTimeline.enter(ThumbnailTimeline.State.PERMIT) else null
        val sem = if (owner.startsWith("thumbnail") && url.host.lowercase() in extraHosts) extra else permits
        try {
            while (true) {
                checkCancelled()
                if (sem.tryAcquire(100, TimeUnit.MILLISECONDS)) break
                if (System.nanoTime() - start > TimeUnit.SECONDS.toNanos(45))
                    throw IOException("media HTTP budget wait exceeded 45 seconds")
            }
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            throw InterruptedIOException("media HTTP budget wait cancelled")
        }
        finally { timelinePermit?.close() }
        val waited = System.nanoTime() - start
        waitMs.addAndGet(waited / 1_000_000)
        threadWait.set(threadWait.get() + waited)
        var connection: HttpURLConnection? = null
        try {
            checkCancelled()
            connection = factory(url)
            checkCancelled()
            val host = url.host.lowercase()
            check(leases.putIfAbsent(connection, Lease(host, owner, onRelease, sem)) == null) { "HTTP connection already owns a lease" }
            val n = current.incrementAndGet()
            peak.accumulateAndGet(n, ::maxOf)
            opens.incrementAndGet()
            byHost.compute(host) { _, value -> (value ?: AtomicInteger()).also { it.incrementAndGet() } }
            Log.i("nuvio-http-budget", "[http-budget] owner=$owner host=$host active=$n cap=${if (sem === extra) "$LIMIT+$EXTRA" else "$LIMIT"} waitMs=${waited / 1_000_000}")
            return connection
        } catch (e: Throwable) {
            try { connection?.disconnect() } finally { sem.release(); onRelease() }
            throw e
        }
    }

    fun close(connection: HttpURLConnection?) {
        if (connection == null) return
        ThumbnailPipelineTimeline.bodyEnd(connection)
        val lease = leases.remove(connection)
        try { connection.disconnect() } finally {
            if (lease != null) {
                byHost.computeIfPresent(lease.host) { _, value -> if (value.decrementAndGet() == 0) null else value }
                current.decrementAndGet()
                lease.sem.release()
                lease.onRelease()
            }
        }
    }
    fun cancelOwner(owner: String) {
        leases.entries.filter { it.value.owner == owner }.forEach { close(it.key) }
    }
    fun snapshot(): String = "http-exchanges active=${current.get()} appPeak=${peak.get()} cap=$LIMIT opens=${opens.get()} permitWaitMs=${waitMs.get()}"
    fun active() = current.get()
    fun peak() = peak.get()
    fun available() = permits.availablePermits()
}

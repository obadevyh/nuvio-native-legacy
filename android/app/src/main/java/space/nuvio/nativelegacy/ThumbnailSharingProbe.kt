package space.nuvio.nativelegacy

import android.content.Context
import android.os.SystemClock
import android.util.Log
import java.io.File

/** Explicit ADB opt-in only. No network, no active-cache writes or deletion.
 * Imports into isolation until strong video identity + remote trust are implemented.
 */
object ThumbnailSharingProbe {
    fun start(context: Context) {
        val root = context.getExternalFilesDir(null) ?: return
        val enabled = File(root, "thumbnail-sharing-test.enabled")
        if (!enabled.isFile) return
        val cache = File(context.filesDir, "dados/miniaturas")
        val output = File(root, "thumbnail-sharing-test")
        Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND)
            Log.i("ThumbShare", "probe enabled; local-only; waiting for completed NVM3 cache")
            // Stop after one success or 30 minutes. Deleting the marker stops polling.
            repeat(180) {
                if (!enabled.isFile) return@Thread
                for (file in cache.listFiles().orEmpty().filter { it.name.matches(Regex("[0-9a-f]{16}\\.nvm")) }
                    .sortedByDescending { it.lastModified() }) {
                    try {
                        val start = SystemClock.elapsedRealtime()
                        val pack = File(output, "latest.nvp")
                        val info = ThumbnailPack.export(file, pack)
                        val exported = SystemClock.elapsedRealtime()
                        val restored = ThumbnailPack.import(pack, File(output, "roundtrip.nvm"))
                        check(info == restored)
                        val report = "PASS tiles=${info.tiles}/${info.slots} durationMs=${info.durationMs} " +
                            "payloadBytes=${info.bytes} packBytes=${pack.length()} exportMs=${exported - start} " +
                            "importMs=${SystemClock.elapsedRealtime() - exported} sha256=${info.sha256} " +
                            "scope=local-roundtrip networkBytes=0 activeCacheModified=false\n"
                        File(output, "result.txt").writeText(report)
                        Log.i("ThumbShare", report.trim())
                        return@Thread
                    } catch (_: Exception) { /* incomplete or changing files retry next poll */ }
                }
                Thread.sleep(10_000)
            }
            Log.i("ThumbShare", "probe stopped: no qualifying completed cache")
        }, "thumbnail-pack-probe").apply { isDaemon = true }.start()
    }
}

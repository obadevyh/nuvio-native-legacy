package space.nuvio.nativelegacy

import android.graphics.Bitmap
import android.media.Image
import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaDataSource
import android.media.MediaExtractor
import android.media.MediaFormat
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.pow

// SEEK-BAR THUMBNAILS MADE ON THIS DEVICE (src/miniatura_android.c is the C side).
//
// One keyframe at a time: seek the container index to the nearest keyframe, hand
// that one sample to a decoder, shrink the frame to 320x180 (JPEG for the bar)
// and to a 32x18 grey signature (OpenSeek's, for checking that a smaller copy of
// the same title lines up with what is playing). Reads go through our own range
// reader (256 KB blocks, small LRU), so nothing is downloaded ahead and the bytes
// are counted. The device's own decoder is tried first (a second session next to
// the player; the Shield allows it), Android's software decoder otherwise.
//
// Called from ONE C worker thread per title, through JNI; handles are ids.
object MiniaturaMotor {
    private const val TAG = "nuvio-miniatura"
    private const val BLK = 256 * 1024
    private const val NBLK = 24
    const val TW = 320
    const val TH = 180
    const val SW = 32
    const val SH = 18

    private class Fonte(url: String, cabecalhos: String) : MediaDataSource() {
        var alvo = url
        val cab = cabecalhos.split('\n').mapNotNull { l ->
            val i = l.indexOf(':'); if (i <= 0) null else l.substring(0, i).trim() to l.substring(i + 1).trim()
        }
        var tamanho = -1L
        var lidos = 0L
        // Big files (4K remuxes): 1 MB blocks, so one keyframe is one or two requests instead of six.
        private var blk = BLK
        private val cache = object : LinkedHashMap<Long, ByteArray>(32, 0.75f, true) {
            override fun removeEldestEntry(e: MutableMap.MutableEntry<Long, ByteArray>?) = size > NBLK * BLK / blk
        }

        init {
            pedir(0, 0)
            if (tamanho > (8L shl 30)) blk = 4 * BLK
        }

        // ONE request at a time for the whole engine, at most ~3 per second, on a kept-alive connection
        // (the stream is read to the end and closed, never disconnect()ed on success, so HttpURLConnection
        // reuses the socket). Add-on proxies (StremThru, AIOStreams) and debrid CDNs limit connections per
        // user; the player's own connections to the SAME link must never be the ones refused.
        private fun pedir(a: Long, b: Long): ByteArray {
            pausar()
            var ultimo: IOException? = null
            for (tentativa in 0 until 2) {
                if (tentativa > 0) { Thread.sleep(2000); pausar() }
                val c = URL(alvo).openConnection() as HttpURLConnection
                val ini = android.os.SystemClock.elapsedRealtime()
                try {
                    c.instanceFollowRedirects = true
                    c.connectTimeout = 15000
                    c.readTimeout = 30000
                    for ((k, v) in cab) c.setRequestProperty(k, v)
                    c.setRequestProperty("Range", "bytes=$a-$b")
                    val code = c.responseCode
                    if (code != 206) throw IOException("http $code")
                    alvo = c.url.toString()   // after debrid/add-on redirects: later reads go straight to the CDN
                    c.getHeaderField("Content-Range")?.substringAfter('/')?.trim()?.toLongOrNull()?.let { tamanho = it }
                    val n = c.contentLength
                    val d = ByteArray(n)
                    var got = 0
                    c.inputStream.use { s -> while (got < n) { val r = s.read(d, got, n - got); if (r < 0) break; got += r } }
                    if (got != n) throw IOException("short read")
                    lidos += got
                    requisicoes++
                    resultado(android.os.SystemClock.elapsedRealtime() - ini, true)
                    return d
                } catch (e: IOException) {
                    ultimo = e
                    resultado(0, false)
                    c.disconnect()
                }
            }
            throw ultimo ?: IOException("read failed")
        }

        private fun bloco(b: Long): ByteArray = cache[b] ?: pedir(b * blk, minOf(tamanho - 1, b * blk + blk - 1)).also { cache[b] = it }

        override fun readAt(pos: Long, buf: ByteArray, off: Int, len: Int): Int {
            if (pos >= tamanho) return -1
            return try {
                var feito = 0
                while (feito < len && pos + feito < tamanho) {
                    val p = pos + feito
                    val d = bloco(p / blk)
                    val ini = (p % blk).toInt()
                    val k = minOf(len - feito, d.size - ini)
                    if (k <= 0) break
                    System.arraycopy(d, ini, buf, off + feito, k)
                    feito += k
                }
                feito
            } catch (e: Exception) {
                erroRede = true   // the extractor only sees "end of data": tell the C side it was the network
                -1
            }
        }
        @Volatile var erroRede = false

        override fun getSize() = tamanho
        override fun close() {}
    }

    private class Sessao(val fonte: Fonte, val ex: MediaExtractor, val fmt: MediaFormat, val mime: String) {
        var dec: MediaCodec? = null
        var soft = false
        var hdr = false
        // Hardware decoder that gives no frame: first again without the low priority, then (<= 1080p) the software one.
        // Measured 22/10: the Shield's H.264 decoder at priority 1 next to playback timed out on every frame.
        var travadas = 0
        var ultimoPts = Long.MIN_VALUE
        var ultimoRes: ByteArray? = null
    }

    private val sessoes = ConcurrentHashMap<Long, Sessao>()
    private var proximo = 1L
    // Request pacing for ALL sessions together, adaptive. Starts at ~6/s (170 ms apart; measured 22/10: one request
    // to a StremThru proxy takes ~0.29 s, so two lanes keep ~6 in flight per second on 2 kept-alive sockets). Any
    // failure or a slow answer (> 1.5 s) doubles the gap at once (up to 1.4 s, ~0.7/s); 20 quick answers in a row
    // take 20 ms off again. The first Shield run showed what crowding the link does: the player's own reconnects
    // were refused.
    private const val INTERVALO_MIN_MS = 170L
    private const val INTERVALO_MAX_MS = 1400L
    private const val LENTO_MS = 1500L
    private var intervalo = INTERVALO_MIN_MS
    private var rapidas = 0
    private var ultimaReq = 0L
    @Volatile private var requisicoes = 0L

    private fun pausar() = synchronized(this) {
        val espera = ultimaReq + intervalo - android.os.SystemClock.elapsedRealtime()
        if (espera > 0) Thread.sleep(espera)
        ultimaReq = android.os.SystemClock.elapsedRealtime()
    }

    private fun resultado(ms: Long, ok: Boolean) = synchronized(this) {
        if (!ok || ms > LENTO_MS) {
            val antes = intervalo
            intervalo = minOf(INTERVALO_MAX_MS, intervalo * 2)
            rapidas = 0
            if (intervalo != antes) Log.i(TAG, "slowing down: ${if (ok) "slow answer ${ms} ms" else "failed request"}, now ${1000 / intervalo}/s")
        } else if (++rapidas >= 20 && intervalo > INTERVALO_MIN_MS) {
            intervalo = maxOf(INTERVALO_MIN_MS, intervalo - 20)
            rapidas = 0
        }
    }

    @JvmStatic
    fun ritmo(): Long = 1000 / intervalo

    @JvmStatic
    fun requisicoes(): Long = requisicoes

    // ---- the tone curve for PQ (HDR10 / Dolby Vision) frames: nits -> SDR grey, OpenSeek's Hable ----
    private val pqLut: IntArray by lazy {
        fun hable(x: Double): Double { val a = 0.15; val b = 0.5; val c = 0.1; val d = 0.2; val e = 0.02; val f = 0.3
            return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f }
        val m1 = 0.1593017578125; val m2 = 78.84375; val c1 = 0.8359375; val c2 = 18.8515625; val c3 = 18.6875
        val white = hable(11.2)
        IntArray(256) { i ->
            val e = (i / 255.0).pow(1 / m2)
            val nits = 10000.0 * (maxOf(e - c1, 0.0) / (c2 - c3 * e)).pow(1 / m1)
            val v = hable(nits / 100.0 * 2.0) / white
            (255.0 * maxOf(0.0, minOf(1.0, v)).pow(1 / 2.2)).toInt()
        }
    }

    @JvmStatic
    fun abrir(url: String, cabecalhos: String): Long = try {
        val f = Fonte(url, cabecalhos)
        val ex = MediaExtractor()
        ex.setDataSource(f)
        var s: Sessao? = null
        for (i in 0 until ex.trackCount) {
            val fm = ex.getTrackFormat(i)
            val m = fm.getString(MediaFormat.KEY_MIME) ?: continue
            if (m.startsWith("video/")) { ex.selectTrack(i); s = Sessao(f, ex, fm, m); break }
        }
        if (s == null) { Log.w(TAG, "open failed: no video track (${ex.trackCount} tracks)"); ex.release(); 0L } else {
            s.hdr = s.fmt.containsKey(MediaFormat.KEY_COLOR_TRANSFER) &&
                s.fmt.getInteger(MediaFormat.KEY_COLOR_TRANSFER) == MediaFormat.COLOR_TRANSFER_ST2084 ||
                s.mime == "video/dolby-vision"
            synchronized(this) { val id = proximo++; sessoes[id] = s; id }
        }
    } catch (e: Exception) {
        Log.w(TAG, "open failed: ${e.javaClass.simpleName}: ${e.message?.take(80)?.replace(Regex("https?://\\S+"), "<url>")}")
        0L
    }

    // [duration ms, width, height, hdr, bytes read so far]
    @JvmStatic
    fun info(h: Long): LongArray {
        val s = sessoes[h] ?: return LongArray(5)
        val d = if (s.fmt.containsKey(MediaFormat.KEY_DURATION)) s.fmt.getLong(MediaFormat.KEY_DURATION) / 1000 else 0L
        return longArrayOf(d, s.fmt.getInteger(MediaFormat.KEY_WIDTH).toLong(), s.fmt.getInteger(MediaFormat.KEY_HEIGHT).toLong(),
            if (s.hdr) 1 else 0, s.fonte.lidos)
    }

    @JvmStatic
    fun lidos(h: Long): Long = sessoes[h]?.fonte?.lidos ?: 0L

    private fun decoder(s: Sessao): MediaCodec? {
        s.dec?.let { return it }
        val altura = s.fmt.getInteger(MediaFormat.KEY_HEIGHT)
        val tentativas = mutableListOf<Pair<String?, Boolean>>()
        if (s.travadas < 2 || altura > 1088) tentativas += null to false
        val mime = if (s.mime == "video/dolby-vision") "video/hevc" else s.mime
        for (ci in MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos) {
            if (ci.isEncoder) continue
            if ((ci.name.startsWith("OMX.google.") || ci.name.startsWith("c2.android.")) && ci.supportedTypes.any { it.equals(mime, true) })
                tentativas += ci.name to true
        }
        for ((nome, soft) in tentativas) {
            var c: MediaCodec? = null
            try {
                c = if (nome == null) MediaCodec.createDecoderByType(s.mime) else MediaCodec.createByCodecName(nome)
                val f = MediaFormat(s.fmt)
                if (s.mime == "video/dolby-vision" && soft) f.setString(MediaFormat.KEY_MIME, "video/hevc")
                // Not real time: the player's decoder keeps the priority (shared hardware on the Shield), unless that
                // already left this decoder without frames once.
                if (s.travadas == 0) f.setInteger(MediaFormat.KEY_PRIORITY, 1)
                c.configure(f, null, null, 0)
                c.start()
                s.dec = c; s.soft = soft
                Log.i(TAG, "decoder ${c.name} (${if (soft) "software" else "hardware"})")
                return c
            } catch (e: Exception) {
                try { c?.release() } catch (_: Exception) {}
            }
        }
        return null
    }

    // Frees the decoder (the container stays open): the next frame opens it again.
    @JvmStatic
    fun soltarDecoder(h: Long) {
        val s = sessoes[h] ?: return
        try { s.dec?.stop() } catch (_: Exception) {}
        try { s.dec?.release() } catch (_: Exception) {}
        s.dec = null
    }

    @JvmStatic
    fun fechar(h: Long) {
        soltarDecoder(h)
        sessoes.remove(h)?.ex?.release()
    }

    // The keyframe nearest `tMs` (closest=true) or the one at/before it.
    // Result: 8 bytes pts ms (big endian) + 1 byte "signature valid" + SW*SH signature bytes (grey, the C side
    // normalises) + the JPEG (only when `jpeg`). null on failure. The same keyframe twice in a row is not
    // decoded again (matching probes the same spot from neighbouring times).
    // 1 when the last quadro/quadroExato of this session failed because a READ failed (back off), 0 otherwise
    // (a frame that would not decode: skip it).
    @JvmStatic
    fun falhaRede(h: Long): Int = if (sessoes[h]?.fonte?.erroRede == true) 1 else 0

    private fun motivo(s: Sessao, tMs: Long, m: String): ByteArray? {
        Log.w(TAG, "no frame at ${tMs / 1000}s: $m${if (s.fonte.erroRede) " (read failed)" else ""}")
        return null
    }

    @JvmStatic
    fun quadro(h: Long, tMs: Long, closest: Boolean, jpeg: Boolean): ByteArray? {
        val s = sessoes[h] ?: return null
        s.fonte.erroRede = false
        try {
            s.ex.seekTo(tMs * 1000, if (closest) MediaExtractor.SEEK_TO_CLOSEST_SYNC else MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            val st = s.ex.sampleTime
            if (st < 0) return motivo(s, tMs, "seek found no sample")
            if (st == s.ultimoPts && !jpeg) s.ultimoRes?.let { return it }
            val dec = decoder(s) ?: return motivo(s, tMs, "no decoder")
            dec.flush()
            val info = MediaCodec.BufferInfo()
            var fed = 0
            var eos = false
            val ini = System.currentTimeMillis()
            while (System.currentTimeMillis() - ini < 4000) {
                if (!eos && fed < 40) {
                    val ib = dec.dequeueInputBuffer(2000)
                    if (ib >= 0) {
                        // Software decoders hand the frame out on end-of-stream; Nvidia's drops it there, so it is fed on.
                        val n = if (s.soft && fed >= 1) -1 else s.ex.readSampleData(dec.getInputBuffer(ib)!!, 0)
                        if (n < 0) { dec.queueInputBuffer(ib, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); eos = true }
                        else { dec.queueInputBuffer(ib, 0, n, s.ex.sampleTime, 0); s.ex.advance(); fed++ }
                    }
                }
                val ob = dec.dequeueOutputBuffer(info, 2000)
                if (ob >= 0) {
                    var res: ByteArray? = null
                    if (info.size > 0) dec.getOutputImage(ob)?.use { img -> res = montar(s, img, info.presentationTimeUs / 1000, jpeg) }
                    dec.releaseOutputBuffer(ob, false)
                    if (res != null) { s.ultimoPts = st; s.ultimoRes = if (jpeg) null else res; return res }
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return motivo(s, tMs, "decoder ended without a frame (fed $fed)")
                }
            }
            soltarDecoder(h)   // stuck: start clean next time, one step further down the decoder ladder
            if (!s.soft && !s.fonte.erroRede) s.travadas++
            return motivo(s, tMs, "decoder timed out (fed $fed), next try: ${if (s.travadas >= 2) "software" else "normal priority"}")
        } catch (e: Exception) {
            soltarDecoder(h)
            return motivo(s, tMs, "error ${e.javaClass.simpleName}")
        }
    }

    // The frame AT `tMs` (not the keyframe): decode forward from the keyframe before it and keep the first frame
    // whose time is >= tMs. Signature only. For the fine pass of the timeline check, on the small copy only
    // (a 720p GOP is cheap to run through; a 4K one is not).
    @JvmStatic
    fun quadroExato(h: Long, tMs: Long): ByteArray? {
        val s = sessoes[h] ?: return null
        s.fonte.erroRede = false
        try {
            s.ex.seekTo(tMs * 1000, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            if (s.ex.sampleTime < 0) return motivo(s, tMs, "exact: seek found no sample")
            val dec = decoder(s) ?: return null
            dec.flush()
            s.ultimoPts = Long.MIN_VALUE
            val info = MediaCodec.BufferInfo()
            var fed = 0
            var eos = false
            val ini = System.currentTimeMillis()
            while (System.currentTimeMillis() - ini < 10000) {
                if (!eos) {
                    val ib = dec.dequeueInputBuffer(2000)
                    if (ib >= 0) {
                        val n = if (fed >= 400) -1 else s.ex.readSampleData(dec.getInputBuffer(ib)!!, 0)
                        if (n < 0) { dec.queueInputBuffer(ib, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); eos = true }
                        else { dec.queueInputBuffer(ib, 0, n, s.ex.sampleTime, 0); s.ex.advance(); fed++ }
                    }
                }
                val ob = dec.dequeueOutputBuffer(info, 2000)
                if (ob >= 0) {
                    var res: ByteArray? = null
                    val pts = info.presentationTimeUs / 1000
                    if (info.size > 0 && pts >= tMs) dec.getOutputImage(ob)?.use { img -> res = montar(s, img, pts, false) }
                    dec.releaseOutputBuffer(ob, false)
                    if (res != null) return res
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return null
                }
            }
            soltarDecoder(h)
            return null
        } catch (e: Exception) {
            soltarDecoder(h)
            return null
        }
    }

    private fun montar(s: Sessao, img: Image, ptsMs: Long, jpeg: Boolean): ByteArray {
        val w = img.width; val h = img.height
        val py = img.planes[0]; val pu = img.planes[1]; val pv = img.planes[2]
        val by = py.buffer; val bu = pu.buffer; val bv = pv.buffer
        val rs = py.rowStride; val ps = py.pixelStride
        val crs = pu.rowStride; val cps = pu.pixelStride
        val off = if (img.format == 0x36) 1 else 0   // YCBCR_P010 (10-bit in 16): the high byte, little endian
        val pq = s.hdr
        fun y(x: Int, yy: Int): Int { val v = by.get(yy * rs + x * ps + off).toInt() and 0xff; return if (pq) pqLut[v] else v }
        // signature: crop 10 % left/right, 16 % top/bottom (letterbox), box-average to 32x18
        val sig = ByteArray(SW * SH)
        val x0 = w / 10; val x1 = w - w / 10; val y0 = h * 16 / 100; val y1 = h - h * 16 / 100
        for (j in 0 until SH) for (i in 0 until SW) {
            val ax = x0 + (x1 - x0) * i / SW; val bx = x0 + (x1 - x0) * (i + 1) / SW
            val ay = y0 + (y1 - y0) * j / SH; val byy = y0 + (y1 - y0) * (j + 1) / SH
            var sum = 0; var n = 0
            var yy = ay
            while (yy < byy) { var xx = ax; while (xx < bx) { sum += y(xx, yy); n++; xx += 2 }; yy += 2 }
            sig[j * SW + i] = (if (n > 0) sum / n else 0).toByte()
        }
        val out = ByteArrayOutputStream(if (jpeg) 16384 else 600)
        for (k in 7 downTo 0) out.write(((ptsMs shr (8 * k)) and 0xff).toInt())
        out.write(1)
        out.write(sig)
        if (jpeg) {
            val px = IntArray(TW * TH)
            // 2x2 samples per output pixel (not one): far less aliasing on edges and text, for 3 more reads each
            val qx = maxOf(1, w / (TW * 4)); val qy = maxOf(1, h / (TH * 4))
            for (ty in 0 until TH) {
                val sy = (ty * h + h / 2) / TH
                val ya = maxOf(0, sy - qy); val yb = minOf(h - 1, sy + qy)
                for (tx in 0 until TW) {
                    val sx = (tx * w + w / 2) / TW
                    val xa = maxOf(0, sx - qx); val xb = minOf(w - 1, sx + qx)
                    val yv = (y(xa, ya) + y(xb, ya) + y(xa, yb) + y(xb, yb) + 2) shr 2
                    val ci = (sy / 2) * crs + (sx / 2) * cps + off
                    var u = (bu.get(ci).toInt() and 0xff) - 128
                    var v = (bv.get(ci).toInt() and 0xff) - 128
                    if (pq) { u /= 2; v /= 2 }   // the tone curve flattens luma; keep colour from oversaturating
                    val c = (yv - 16).coerceAtLeast(0) * 298
                    val r = ((c + 409 * v + 128) shr 8).coerceIn(0, 255)
                    val g = ((c - 100 * u - 208 * v + 128) shr 8).coerceIn(0, 255)
                    val b = ((c + 516 * u + 128) shr 8).coerceIn(0, 255)
                    px[ty * TW + tx] = (0xff shl 24) or (r shl 16) or (g shl 8) or b
                }
            }
            val bmp = Bitmap.createBitmap(px, TW, TH, Bitmap.Config.ARGB_8888)
            bmp.compress(Bitmap.CompressFormat.JPEG, 80, out)
            bmp.recycle()
        }
        return out.toByteArray()
    }
}

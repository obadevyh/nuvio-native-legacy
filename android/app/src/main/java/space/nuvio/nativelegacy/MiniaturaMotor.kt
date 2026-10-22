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
// that one sample to a decoder, shrink the frame to 480x270 (JPEG for the bar)
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
    const val TW = 480   // shown at 576x324 in the seek preview (player.c); was 320x180
    const val TH = 270
    const val SW = 32
    const val SH = 18

    // `manter` = keep the socket alive between requests (the copy, read all the time). The PLAYING file is read
    // only for the check: each of its requests closes its socket, so no idle connection to the player's own link
    // stays open next to the player's (debrid CDNs limit connections per user; measured 22/10 on TorBox: past the
    // limit, the player's reconnects are refused and playback stops).
    private class Fonte(url: String, cabecalhos: String, val manter: Boolean) : MediaDataSource() {
        var alvo = url
        @Volatile var recusado = false    // a connection was refused or timed out: stop reading this title at all
        val cab = cabecalhos.split('\n').mapNotNull { l ->
            val i = l.indexOf(':'); if (i <= 0) null else l.substring(0, i).trim() to l.substring(i + 1).trim()
        }
        var tamanho = -1L
        var lidos = 0L
        // Reads start where the extractor asks (the keyframe), not at a block boundary: a 720p keyframe (~100-150 KB)
        // is ONE request of 384 KB instead of two aligned 256 KB blocks half the time (measured 1.46 requests per
        // thumbnail with blocks). Big files (4K remuxes, keyframes of 1.5-3 MB): 2 MB windows.
        private var janela = 384 * 1024
        private var limite = 6L shl 20
        private val segs = ArrayList<Pair<Long, ByteArray>>()   // most recently used last
        private var guardados = 0L

        init {
            pedir(0, 0)
            if (tamanho > (8L shl 30)) { janela = 2 shl 20; limite = 12L shl 20 }
        }

        // The copy's window follows its keyframes: ~1.6x the largest of the recent ones (+32 KB for the container's
        // block headers), 128-384 KB. Measured 22/10: a fixed 384 KB read ~0.43 MB per 720p thumbnail, about 3x the
        // keyframe. A keyframe bigger than the window simply costs a second request, never a wrong frame.
        // Sized from the TYPICAL keyframe (running mean x 1.3 + 32 KB, 96-384 KB), not 1.6x the largest: measured
        // 22/10, an H.264 copy read ~0.5 MB per thumbnail for ~0.2 MB keyframes. A bigger keyframe costs one more
        // small request, never a wrong frame. +64 KB of margin for the block headers around it.
        private var media = 0
        fun ajustarJanela(amostra: Int) {
            somaKf += amostra; nKf++
            if (!manter || tamanho > (8L shl 30)) return
            media = if (media == 0) amostra else (media * 7 + amostra) / 8
            janela = (media * 13 / 10 + 64 * 1024).coerceIn(128 * 1024, 384 * 1024)
        }
        // keyframes per multi-range request: as many as ~4 MB of windows (4-32)
        fun loteIdeal(): Int = ((4 shl 20) / janela).coerceIn(4, 32)

        private fun guardar(a: Long, d: ByteArray) {
            segs.add(a to d); guardados += d.size
            while (guardados > limite && segs.size > 1) guardados -= segs.removeAt(0).second.size
        }
        private fun temByte(p: Long) = segs.any { p >= it.first && p < it.first + it.second.size }

        // ---- MULTI-RANGE BATCHES (measured 22/10 on TorBox's Nexus servers: one GET with 8 ranges came back as a
        // 206 multipart/byteranges with all 8 parts in 373 ms, vs ~190 ms for EACH single range) ----
        // The keyframes' byte positions come from the Matroska index (Cues); a batch of thumbnails is ONE request on
        // the same single connection, stored here, and the extractor's own seek then finds the bytes in memory.
        @Volatile var multiOk = true          // false after one answer that was not multipart: single ranges from then on
        private var cues: LongArray? = null   // [time ms, absolute cluster position, relative block position] x n
        private var cuesTentado = false

        private fun ler(p: Long, n: Int): ByteArray? {
            if (p < 0 || p >= tamanho) return null
            val b = ByteArray(minOf(n.toLong(), tamanho - p).toInt())
            var f = 0
            while (f < b.size) { val r = readAt(p + f, b, f, b.size - f); if (r <= 0) return null; f += r }
            return b
        }
        private fun vint(b: ByteArray, i: Int, id: Boolean): Pair<Long, Int>? {   // (value, length)
            if (i >= b.size) return null
            val first = b[i].toInt() and 0xff
            var len = 1; var mask = 0x80
            while (len <= 8 && first and mask == 0) { len++; mask = mask shr 1 }
            if (len > 8 || i + len > b.size) return null
            var v = (if (id) first else first and (mask - 1)).toLong()
            for (k in 1 until len) v = (v shl 8) or (b[i + k].toLong() and 0xff)
            if (!id && v == (1L shl (7 * len)) - 1) v = -1   // unknown size
            return v to len
        }
        private fun uint(b: ByteArray, i: Int, n: Int): Long { var v = 0L; for (k in 0 until n) v = (v shl 8) or (b[i + k].toLong() and 0xff); return v }

        // Segment -> SeekHead -> Cues (+ Info's TimestampScale, Tracks' video track number). Null when not Matroska
        // or anything is unexpected: the batches are then simply not used.
        private fun carregarCues(): LongArray? {
            if (cuesTentado) return cues
            cuesTentado = true
            try {
                val h = ler(0, 64 * 1024) ?: return null
                val (ebmlId, l0) = vint(h, 0, true) ?: return null
                if (ebmlId != 0x1A45DFA3L) return null
                val (ebmlSz, l1) = vint(h, l0, false) ?: return null
                var i = (l0 + l1 + ebmlSz).toInt()
                val (segId, s0) = vint(h, i, true) ?: return null
                if (segId != 0x18538067L) return null
                val (_, s1) = vint(h, i + s0, false) ?: return null
                val segIni = (i + s0 + s1).toLong()
                i = segIni.toInt()
                var cuesPos = -1L; var infoPos = -1L; var tracksPos = -1L
                // SeekHead is the first child of Segment in practice (mkvmerge, ffmpeg)
                val (shId, h0) = vint(h, i, true) ?: return null
                val (shSz, h1) = vint(h, i + h0, false) ?: return null
                if (shId != 0x114D9B74L) return null
                var j = i + h0 + h1
                val fim = j + shSz.toInt()
                while (j < fim && j < h.size) {
                    val (sid, a0) = vint(h, j, true) ?: break
                    val (ssz, a1) = vint(h, j + a0, false) ?: break
                    var k = j + a0 + a1
                    val kfim = k + ssz.toInt()
                    var qual = 0L; var onde = -1L
                    if (sid == 0x4DBBL) while (k < kfim) {
                        val (eid, e0) = vint(h, k, true) ?: break
                        val (esz, e1) = vint(h, k + e0, false) ?: break
                        if (eid == 0x53ABL) qual = uint(h, k + e0 + e1, esz.toInt())
                        if (eid == 0x53ACL) onde = uint(h, k + e0 + e1, esz.toInt())
                        k += e0 + e1 + esz.toInt()
                    }
                    when (qual) { 0x1C53BB6BL -> cuesPos = segIni + onde; 0x1549A966L -> infoPos = segIni + onde; 0x1654AE6BL -> tracksPos = segIni + onde }
                    j = kfim
                }
                if (cuesPos < 0) return null
                var escala = 1_000_000L
                if (infoPos >= 0) ler(infoPos, 4096)?.let { b ->
                    val (_, i0) = vint(b, 0, true) ?: return@let
                    val (isz, i1) = vint(b, i0, false) ?: return@let
                    var k = i0 + i1
                    while (k < minOf(b.size, i0 + i1 + isz.toInt())) {
                        val (eid, e0) = vint(b, k, true) ?: break
                        val (esz, e1) = vint(b, k + e0, false) ?: break
                        if (eid == 0x2AD7B1L) escala = uint(b, k + e0 + e1, esz.toInt())
                        k += e0 + e1 + esz.toInt()
                    }
                }
                var trilhaVideo = -1L
                if (tracksPos >= 0) ler(tracksPos, 64 * 1024)?.let { b ->
                    val (_, t0) = vint(b, 0, true) ?: return@let
                    val (tsz, t1) = vint(b, t0, false) ?: return@let
                    var k = t0 + t1
                    while (k < minOf(b.size, t0 + t1 + tsz.toInt()) && trilhaVideo < 0) {
                        val (eid, e0) = vint(b, k, true) ?: break
                        val (esz, e1) = vint(b, k + e0, false) ?: break
                        if (eid == 0xAEL) {
                            var m = k + e0 + e1; var num = -1L; var tipo = -1L
                            while (m < k + e0 + e1 + esz.toInt() && m < b.size) {
                                val (fid, f0) = vint(b, m, true) ?: break
                                val (fsz, f1) = vint(b, m + f0, false) ?: break
                                if (fid == 0xD7L) num = uint(b, m + f0 + f1, fsz.toInt())
                                if (fid == 0x83L) tipo = uint(b, m + f0 + f1, fsz.toInt())
                                m += f0 + f1 + fsz.toInt()
                            }
                            if (tipo == 1L) trilhaVideo = num
                        }
                        k += e0 + e1 + esz.toInt()
                    }
                }
                val ch = ler(cuesPos, 16) ?: return null
                val (cid, c0) = vint(ch, 0, true) ?: return null
                val (csz, c1) = vint(ch, c0, false) ?: return null
                if (cid != 0x1C53BB6BL || csz <= 0 || csz > (16L shl 20)) return null
                val b = ler(cuesPos + c0 + c1, csz.toInt()) ?: return null
                val out = ArrayList<Long>()
                var k = 0
                while (k < b.size) {
                    val (pid, p0) = vint(b, k, true) ?: break
                    val (psz, p1) = vint(b, k + p0, false) ?: break
                    if (pid == 0xBBL) {
                        var m = k + p0 + p1; var t = -1L
                        val mfim = m + psz.toInt()
                        while (m < mfim && m < b.size) {
                            val (eid, e0) = vint(b, m, true) ?: break
                            val (esz, e1) = vint(b, m + e0, false) ?: break
                            if (eid == 0xB3L) t = uint(b, m + e0 + e1, esz.toInt())
                            if (eid == 0xB7L) {
                                var q = m + e0 + e1; var trilha = -1L; var cl = -1L; var rel = 0L
                                while (q < m + e0 + e1 + esz.toInt()) {
                                    val (gid, g0) = vint(b, q, true) ?: break
                                    val (gsz, g1) = vint(b, q + g0, false) ?: break
                                    when (gid) {
                                        0xF7L -> trilha = uint(b, q + g0 + g1, gsz.toInt())
                                        0xF1L -> cl = uint(b, q + g0 + g1, gsz.toInt())
                                        0xF0L -> rel = uint(b, q + g0 + g1, gsz.toInt())
                                    }
                                    q += g0 + g1 + gsz.toInt()
                                }
                                if (t >= 0 && cl >= 0 && (trilhaVideo < 0 || trilha == trilhaVideo)) {
                                    out += t * escala / 1_000_000L; out += segIni + cl; out += rel
                                }
                            }
                            m += e0 + e1 + esz.toInt()
                        }
                    }
                    k += p0 + p1 + psz.toInt()
                }
                if (out.size < 30) return null
                cues = out.toLongArray()
                Log.i(TAG, "keyframe index: ${out.size / 3} entries (Matroska Cues), multi-range batches on")
                return cues
            } catch (e: Exception) {
                return null
            }
        }

        // Prefetch the keyframes nearest these times (the extractor's SEEK_TO_CLOSEST_SYNC picks the same ones) in ONE
        // multi-range request. Returns how many were fetched, 0 nothing to do, -1 batches not possible for this file.
        fun prefetch(tempos: LongArray): Int {
            if (!multiOk || !manter) return -1
            val c = carregarCues() ?: return -1
            val n = c.size / 3
            val faixas = ArrayList<LongArray>()
            for (t in tempos) {
                var lo = 0; var hi = n - 1
                while (lo < hi) { val mid = (lo + hi) / 2; if (c[mid * 3] < t) lo = mid + 1 else hi = mid }
                val k = if (lo > 0 && t - c[(lo - 1) * 3] < c[lo * 3] - t) lo - 1 else lo
                val ini = c[k * 3 + 1]
                if (temByte(ini)) continue
                val fim = minOf(tamanho - 1, ini + c[k * 3 + 2] + janela - 1)
                if (faixas.none { it[0] == ini }) faixas += longArrayOf(ini, fim)
            }
            if (faixas.isEmpty()) return 0
            faixas.sortBy { it[0] }
            val partes = synchronized(VOO) { pedirVarios(faixas) } ?: return -1
            for ((a, d) in partes) guardar(a, d)
            return partes.size
        }

        private fun pedirVarios(faixas: List<LongArray>): List<Pair<Long, ByteArray>>? {
            if (recusado) throw IOException("refused earlier")
            pausar()
            val c = URL(alvo).openConnection() as HttpURLConnection
            val ini = android.os.SystemClock.elapsedRealtime()
            try {
                c.instanceFollowRedirects = true
                c.connectTimeout = 6000
                c.readTimeout = 6000
                for ((k, v) in cab) c.setRequestProperty(k, v)
                c.setRequestProperty("Accept-Encoding", "identity")
                c.setRequestProperty("Range", "bytes=" + faixas.joinToString(",") { "${it[0]}-${it[1]}" })
                val code = c.responseCode
                val tipo = c.contentType ?: ""
                if (code != 206 || !tipo.lowercase().startsWith("multipart/byteranges")) {
                    multiOk = false   // single range or whole file: never again for this file (and read nothing more)
                    Log.w(TAG, "multi-range not served here (http $code, $tipo): single ranges from now on")
                    c.disconnect()
                    return null
                }
                val fronteira = Regex("boundary=\"?([^\";]+)").find(tipo)?.groupValues?.get(1) ?: run { multiOk = false; c.disconnect(); return null }
                val corpo = c.inputStream.use { it.readBytes() }
                lidos += corpo.size
                lidosTotal += corpo.size
                requisicoes++
                msRede += android.os.SystemClock.elapsedRealtime() - ini
                nLotes++; bytesLotes += corpo.size; kfLotes += faixas.size
                resultado(android.os.SystemClock.elapsedRealtime() - ini, true, corpo.size)
                val partes = ArrayList<Pair<Long, ByteArray>>()
                val marca = ("--$fronteira").toByteArray()
                var p = indice(corpo, marca, 0)
                while (p >= 0) {
                    val cabFim = indice(corpo, "\r\n\r\n".toByteArray(), p)
                    if (cabFim < 0) break
                    val cabTxt = String(corpo, p, cabFim - p, Charsets.ISO_8859_1)
                    val m = Regex("Content-Range:\\s*bytes (\\d+)-(\\d+)/", RegexOption.IGNORE_CASE).find(cabTxt) ?: break
                    val a = m.groupValues[1].toLong(); val b = m.groupValues[2].toLong()
                    val dIni = cabFim + 4; val dLen = (b - a + 1).toInt()
                    if (dIni + dLen > corpo.size) break
                    partes += a to corpo.copyOfRange(dIni, dIni + dLen)
                    p = indice(corpo, marca, dIni + dLen)
                }
                if (partes.isEmpty()) { multiOk = false; return null }
                return partes
            } catch (e: IOException) {
                resultado(0, false)
                c.disconnect()
                if (e is java.net.ConnectException || e is java.net.SocketTimeoutException) {
                    recusado = true
                    Log.w(TAG, "connection ${if (e is java.net.ConnectException) "refused" else "timed out"}: stopping this title")
                }
                throw e
            }
        }

        private fun indice(h: ByteArray, agulha: ByteArray, de: Int): Int {
            var i = de
            while (i <= h.size - agulha.size) {
                var k = 0
                while (k < agulha.size && h[i + k] == agulha[k]) k++
                if (k == agulha.size) return i
                i++
            }
            return -1
        }

        private fun segmento(p: Long): Pair<Long, ByteArray> {
            for (i in segs.indices.reversed()) {
                val s = segs[i]
                if (p >= s.first && p < s.first + s.second.size) { if (i != segs.size - 1) { segs.removeAt(i); segs.add(s) }; return s }
            }
            // A read that continues right where a cached segment ends is a FORWARD decode (an exact frame: the
            // check's fine pass, the gap fills): 1 MB at a time then, not a keyframe-sized window. Measured 23/10:
            // with ~190 KB windows, exact frames needed ~25 requests each, queued behind the batches, and timed out.
            val seguido = segs.any { it.first + it.second.size == p }
            val n = if (seguido && manter) maxOf(janela, 1 shl 20) else janela
            val d = pedir(p, minOf(tamanho - 1, p + n - 1))
            guardar(p, d)
            return p to d
        }

        // ONE request at a time for the whole engine, at most ~4 per second, on one kept-alive connection
        // (the stream is read to the end and closed, never disconnect()ed on success, so HttpURLConnection
        // reuses the socket). A refused or silent connection is never retried: the title stops (see recusado).
        // Every session (both lanes, the check's readers) waits for this: ONE request in flight for the whole engine,
        // so the lanes share one pooled socket per host and one lane decodes while the other's request is on the wire.
        private fun pedir(a: Long, b: Long): ByteArray = synchronized(VOO) { pedirUm(a, b) }

        private fun pedirUm(a: Long, b: Long): ByteArray {
            if (recusado) throw IOException("refused earlier")
            pausar()
            var ultimo: IOException? = null
            for (tentativa in 0 until 2) {
                if (tentativa > 0) { Thread.sleep(2000); pausar() }
                val c = URL(alvo).openConnection() as HttpURLConnection
                val ini = android.os.SystemClock.elapsedRealtime()
                try {
                    c.instanceFollowRedirects = true
                    c.connectTimeout = 6000
                    c.readTimeout = 6000
                    for ((k, v) in cab) c.setRequestProperty(k, v)
                    c.setRequestProperty("Range", "bytes=$a-$b")
                    if (!manter) c.setRequestProperty("Connection", "close")
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
                    lidosTotal += got
                    requisicoes++
                    msRede += android.os.SystemClock.elapsedRealtime() - ini
                    resultado(android.os.SystemClock.elapsedRealtime() - ini, true, got)
                    if (!manter) c.disconnect()
                    return d
                } catch (e: IOException) {
                    ultimo = e
                    resultado(0, false)
                    c.disconnect()
                    if (e is java.net.ConnectException || e is java.net.SocketTimeoutException) {
                        recusado = true
                        Log.w(TAG, "connection ${if (e is java.net.ConnectException) "refused" else "timed out"}: stopping this title")
                        break
                    }
                }
            }
            throw ultimo ?: IOException("read failed")
        }

        override fun readAt(pos: Long, buf: ByteArray, off: Int, len: Int): Int {
            if (pos >= tamanho) return -1
            return try {
                var feito = 0
                while (feito < len && pos + feito < tamanho) {
                    val p = pos + feito
                    val (ini0, d) = segmento(p)
                    val ini = (p - ini0).toInt()
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
        // Codec errors (IllegalState/CodecException) on this file: at 2 the file is given up. Measured 22/10: a copy
        // whose frames threw at once made 8 decoders in ~1 s, and the player's own decoder failed (4003) next to it.
        var errosCodec = 0
        // reused per session: the frame's planes copied in one go (see montar)
        var yArr = ByteArray(0); var uArr = ByteArray(0); var vArr = ByteArray(0)
        var ultimoPts = Long.MIN_VALUE
        var ultimoRes: ByteArray? = null
    }

    private val sessoes = ConcurrentHashMap<Long, Sessao>()
    private var proximo = 1L
    private val VOO = Any()

    // Request pacing for ALL sessions together, adaptive. At most ~4/s (250 ms apart; one request takes ~0.29 s, so
    // in practice the single in-flight request is the limit, ~3.4/s). Two parallel connections next to the player
    // got its reconnects refused by TorBox (22/10), so there is never more than one. Any
    // failure or a slow answer (> 1.5 s) doubles the gap at once (up to 1.4 s, ~0.7/s); 20 quick answers in a row
    // take 20 ms off again. The first Shield run showed what crowding the link does: the player's own reconnects
    // were refused.
    private const val INTERVALO_MIN_MS = 250L
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

    // "Slow" is judged against the size: 1.5 s plus ~0.7 s per MB (a 2 MB 4K window at 2.7 s is a normal download,
    // not a struggling link; measured 22/10, it cut the pace to 1/s for the rest of a 3 h film). Recovery is
    // multiplicative: 10 good answers take 25 % off the gap, so ~50 requests from the floor back to full pace.
    private fun resultado(ms: Long, ok: Boolean, bytes: Int = 0) = synchronized(this) {
        if (ok && bytes <= 1) return@synchronized          // the 0-0 size probe: its latency says nothing about the link
        if (!ok || ms > LENTO_MS + bytes / 1500) {
            val antes = intervalo
            intervalo = minOf(INTERVALO_MAX_MS, intervalo * 2)
            rapidas = 0
            if (intervalo != antes) Log.i(TAG, "slowing down: ${if (ok) "slow answer ${ms} ms for ${bytes / 1024} KB" else "failed request"}, now ${1000 / intervalo}/s")
        } else if (++rapidas >= 10 && intervalo > INTERVALO_MIN_MS) {
            intervalo = maxOf(INTERVALO_MIN_MS, intervalo * 3 / 4)
            rapidas = 0
        }
    }

    @JvmStatic
    fun ritmo(): Long = 1000 / intervalo

    @JvmStatic
    fun requisicoes(): Long = requisicoes

    @Volatile private var criados = 0L

    // For the per-title summary: [requests sent, decoders created] since the app started (the C side takes deltas).
    @JvmStatic
    // [requests, decoders created, bytes, network ms, frame ms, batches, batch bytes, batch keyframes, keyframe bytes, keyframes]
    fun contadores(): LongArray = longArrayOf(requisicoes, criados, lidosTotal, msRede, msQuadros, nLotes, bytesLotes, kfLotes, somaKf, nKf)
    @Volatile private var lidosTotal = 0L
    @Volatile private var msRede = 0L
    @Volatile private var msQuadros = 0L
    @Volatile private var nLotes = 0L
    @Volatile private var bytesLotes = 0L
    @Volatile private var kfLotes = 0L
    @Volatile private var somaKf = 0L
    @Volatile private var nKf = 0L

    @JvmStatic
    fun lote(h: Long): Int = sessoes[h]?.fonte?.loteIdeal() ?: 8

    // The link AFTER the add-on/debrid redirect (the CDN address): later sessions on the same copy open it directly,
    // so the add-on mints one debrid link per copy, not one per session.
    @JvmStatic
    fun alvo(h: Long): String = sessoes[h]?.fonte?.alvo ?: ""

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
    fun abrir(url: String, cabecalhos: String, manter: Boolean): Long = try {
        val f = Fonte(url, cabecalhos, manter)
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

    // Prefetch the keyframes for these copy times in one multi-range request. >= 0 fetched, -1 not possible for this
    // file (not Matroska, no index, server answered single ranges), -2 the connection was refused (stop the title).
    @JvmStatic
    fun preparar(h: Long, tempos: LongArray): Int {
        val f = sessoes[h]?.fonte ?: return -1
        return try { f.prefetch(tempos) } catch (e: IOException) { if (f.recusado) -2 else -1 }
    }

    // At most one NEW decoder every 2 s for the whole engine: decoders are a shared resource of this process, and a
    // burst of them next to the player is what took the player's away (22/10).
    private var ultimaCriacao = 0L
    private fun decoder(s: Sessao): MediaCodec? {
        s.dec?.let { return it }
        if (s.errosCodec >= 2) return null
        synchronized(this) {
            val espera = ultimaCriacao + 2000 - android.os.SystemClock.elapsedRealtime()
            if (espera > 0) Thread.sleep(espera)
            ultimaCriacao = android.os.SystemClock.elapsedRealtime()
        }
        val altura = s.fmt.getInteger(MediaFormat.KEY_HEIGHT)
        val tentativas = mutableListOf<Pair<String?, Boolean>>()
        val mime = if (s.mime == "video/dolby-vision") "video/hevc" else s.mime
        val softs = MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos.filter { ci ->
            !ci.isEncoder && (ci.name.startsWith("OMX.google.") || ci.name.startsWith("c2.android.")) && ci.supportedTypes.any { it.equals(mime, true) }
        }.map { it.name to true }
        // Up to 1080p the CPU decodes a keyframe in ~50-100 ms (Shield): software FIRST, so thumbnails never take a
        // hardware session next to the player (the Shield's H.264 one gave no frames there, 22/10). Above: hardware.
        if (altura <= 1088) { tentativas += softs; tentativas += null to false }
        else { if (s.travadas < 2) tentativas += null to false; tentativas += softs }
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
                criados++
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
    fun falhaRede(h: Long): Int {
        val f = sessoes[h]?.fonte ?: return 0
        return if (f.recusado) 2 else if (f.erroRede) 1 else 0   // 2 = refused/silent: stop the title
    }

    private fun motivo(s: Sessao, tMs: Long, m: String): ByteArray? {
        Log.w(TAG, "no frame at ${tMs / 1000}s: $m${if (s.fonte.erroRede) " (read failed)" else ""}")
        return null
    }

    @JvmStatic
    fun quadro(h: Long, tMs: Long, closest: Boolean, jpeg: Boolean): ByteArray? {
        val t0 = android.os.SystemClock.elapsedRealtime()
        try { return quadroUm(h, tMs, closest, jpeg) } finally { msQuadros += android.os.SystemClock.elapsedRealtime() - t0 }
    }

    private fun quadroUm(h: Long, tMs: Long, closest: Boolean, jpeg: Boolean): ByteArray? {
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
                        // HEVC gets ONE more sample first: Android's software HEVC decoder sometimes ended without a
                        // frame on the keyframe alone (4 of 612, 22/10); the first frame out is still the keyframe.
                        val antesDoFim = if (s.mime == "video/hevc" || s.mime == "video/dolby-vision") 2 else 1
                        val n = if (s.soft && fed >= antesDoFim) -1 else s.ex.readSampleData(dec.getInputBuffer(ib)!!, 0)
                        if (n < 0) { dec.queueInputBuffer(ib, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); eos = true }
                        else {
                            if (fed == 0) s.fonte.ajustarJanela(n)   // the keyframe's real size sizes the next reads
                            dec.queueInputBuffer(ib, 0, n, s.ex.sampleTime, 0)
                            fed++
                            // No advance() after the LAST sample fed (software path): on Matroska it reads on through
                            // the audio blocks to the next video frame, past the prefetched window. Measured 23/10:
                            // ~1 extra request per thumbnail (847 requests for 25 batches on a 2 h film).
                            if (!s.soft || fed < antesDoFim) s.ex.advance()
                        }
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
            if (e is IllegalStateException || e is MediaCodec.CodecException) s.errosCodec++
            return motivo(s, tMs, "error ${e.javaClass.simpleName}${if (s.errosCodec >= 2) ": decoder given up for this file" else ""}")
        }
    }

    // The frame AT `tMs` (not the keyframe): decode forward from the keyframe before it and keep the first frame
    // whose time is >= tMs. Signature only. For the fine pass of the timeline check, on the small copy only
    // (a 720p GOP is cheap to run through; a 4K one is not).
    @JvmStatic
    fun quadroExato(h: Long, tMs: Long, jpeg: Boolean): ByteArray? {
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
            while (System.currentTimeMillis() - ini < 15000) {   // forward decode through a GOP: reads queue behind the batches
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
                    if (info.size > 0 && pts >= tMs) dec.getOutputImage(ob)?.use { img -> res = montar(s, img, pts, jpeg) }
                    dec.releaseOutputBuffer(ob, false)
                    if (res != null) return res
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return null
                }
            }
            soltarDecoder(h)
            return null
        } catch (e: Exception) {
            soltarDecoder(h)
            if (e is IllegalStateException || e is MediaCodec.CodecException) s.errosCodec++
            return null
        }
    }

    // The frame on screen (NvPlayer.assinaturaTela), in the reply format of quadro() without a JPEG. Luma from RGB
    // (Rec. 709 weights), the same crop and 32x18 grid as montar(); the C side removes the mean and normalises, so
    // the RGB-vs-Y range difference does not matter.
    @JvmStatic
    fun tela(): ByteArray? = NvPlayer.assinaturaTela()

    fun assinaturaDe(b: Bitmap, posMs: Long): ByteArray {
        val w = b.width; val h = b.height
        val px = IntArray(w * h)
        b.getPixels(px, 0, w, 0, 0, w, h)
        val sig = ByteArray(SW * SH)
        val x0 = w / 10; val x1 = w - w / 10; val y0 = h * 16 / 100; val y1 = h - h * 16 / 100
        for (j in 0 until SH) for (i in 0 until SW) {
            val ax = x0 + (x1 - x0) * i / SW; val bx = maxOf(ax + 1, x0 + (x1 - x0) * (i + 1) / SW)
            val ay = y0 + (y1 - y0) * j / SH; val by = maxOf(ay + 1, y0 + (y1 - y0) * (j + 1) / SH)
            var sum = 0; var n = 0
            for (yy in ay until by) for (xx in ax until bx) {
                val c = px[yy * w + xx]
                sum += (((c shr 16) and 0xff) * 54 + ((c shr 8) and 0xff) * 183 + (c and 0xff) * 19) shr 8
                n++
            }
            sig[j * SW + i] = (if (n > 0) sum / n else 0).toByte()
        }
        val out = ByteArrayOutputStream(600)
        for (k in 7 downTo 0) out.write(((posMs shr (8 * k)) and 0xff).toInt())
        out.write(1)
        out.write(sig)
        return out.toByteArray()
    }

    private fun montar(s: Sessao, img: Image, ptsMs: Long, jpeg: Boolean): ByteArray {
        // The VISIBLE area: decoders pad (1920x1088 for 1080p) and report the picture in cropRect.
        val cr = img.cropRect
        val cx = if (cr.width() > 0) cr.left else 0
        val cy = if (cr.height() > 0) cr.top else 0
        val w = if (cr.width() > 0) cr.width() else img.width
        val h = if (cr.height() > 0) cr.height() else img.height
        val py = img.planes[0]; val pu = img.planes[1]; val pv = img.planes[2]
        // Each plane copied ONCE into a reused array, then plain array reads: ByteBuffer.get() per sample cost ~0.4 s of
        // CPU per 480x270 thumbnail on the Shield (23/10: frames 345 s vs network 108 s on a 2 h film).
        fun copia(b: java.nio.ByteBuffer, atual: ByteArray): ByteArray {
            val d = b.duplicate(); d.position(0)
            val arr = if (atual.size >= d.limit()) atual else ByteArray(d.limit())
            d.get(arr, 0, d.limit())
            return arr
        }
        s.yArr = copia(py.buffer, s.yArr)
        val ya8 = s.yArr
        val rs = py.rowStride; val ps = py.pixelStride
        val crs = pu.rowStride; val cps = pu.pixelStride
        val off = if (img.format == 0x36) 1 else 0   // YCBCR_P010 (10-bit in 16): the high byte, little endian
        val pq = s.hdr
        fun y(x: Int, yy: Int): Int { val v = ya8[(yy + cy) * rs + (x + cx) * ps + off].toInt() and 0xff; return if (pq) pqLut[v] else v }
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
            s.uArr = copia(pu.buffer, s.uArr); s.vArr = copia(pv.buffer, s.vArr)
            val ua8 = s.uArr; val va8 = s.vArr
            // The picture keeps ITS shape inside the 16:9 thumbnail, with black bars, as it looks on the TV. Measured
            // 22/10: a 1280x536 (2.39:1) copy squeezed into 480x270 stretched everything vertically.
            val px = IntArray(TW * TH) { 0xff000000.toInt() }
            val dw: Int; val dh: Int
            if (w.toLong() * TH >= h.toLong() * TW) { dw = TW; dh = ((TW.toLong() * h / w).toInt() / 2 * 2).coerceIn(2, TH) }
            else { dh = TH; dw = ((TH.toLong() * w / h).toInt() / 2 * 2).coerceIn(2, TW) }
            val ox = (TW - dw) / 2; val oy = (TH - dh) / 2
            // 2x2 samples per output pixel (not one): far less aliasing on edges and text, for 3 more reads each
            val qx = maxOf(1, w / (dw * 4)); val qy = maxOf(1, h / (dh * 4))
            for (ty in 0 until dh) {
                val sy = (ty * h + h / 2) / dh
                val ya = maxOf(0, sy - qy); val yb = minOf(h - 1, sy + qy)
                for (tx in 0 until dw) {
                    val sx = (tx * w + w / 2) / dw
                    val xa = maxOf(0, sx - qx); val xb = minOf(w - 1, sx + qx)
                    val yv = (y(xa, ya) + y(xb, ya) + y(xa, yb) + y(xb, yb) + 2) shr 2
                    val ci = ((sy + cy) / 2) * crs + ((sx + cx) / 2) * cps + off
                    var u = (ua8[ci].toInt() and 0xff) - 128
                    var v = (va8[ci].toInt() and 0xff) - 128
                    if (pq) { u /= 2; v /= 2 }   // the tone curve flattens luma; keep colour from oversaturating
                    val c = (yv - 16).coerceAtLeast(0) * 298
                    val r = ((c + 409 * v + 128) shr 8).coerceIn(0, 255)
                    val g = ((c - 100 * u - 208 * v + 128) shr 8).coerceIn(0, 255)
                    val b = ((c + 516 * u + 128) shr 8).coerceIn(0, 255)
                    px[(oy + ty) * TW + ox + tx] = (0xff shl 24) or (r shl 16) or (g shl 8) or b
                }
            }
            val bmp = Bitmap.createBitmap(px, TW, TH, Bitmap.Config.ARGB_8888)
            bmp.compress(Bitmap.CompressFormat.JPEG, 80, out)
            bmp.recycle()
        }
        return out.toByteArray()
    }
}

package space.nuvio.nativelegacy

import org.junit.Assert.*
import org.junit.Test
import java.io.*
import java.nio.file.Files

class ThumbnailPackTest {
    private fun cache(file: File, complete: Int = 1, keys: List<Int> = listOf(0, 3, 6)) {
        val jpg = java.util.Base64.getDecoder().decode("/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAEAAQDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5/ooooA//2Q==")
        DataOutputStream(file.outputStream()).use { out ->
            out.writeInt(0x4e564d33); out.writeInt(7); out.writeInt(60_000); out.writeInt(complete)
            for (k in keys) {
                out.writeInt(k); out.writeInt(k * 10_000 - if (k > 0) 350 else 0)
                out.writeInt(jpg.size); out.write(jpg)
            }
        }
    }
    private fun run(block: (File) -> Unit) {
        val dir = Files.createTempDirectory("thumbnail-pack-test").toFile()
        try { block(dir) } finally { dir.deleteRecursively() }
    }
    private fun rejects(action: () -> Unit) {
        try { action(); fail("must reject invalid pack") } catch (_: IllegalArgumentException) { } catch (_: EOFException) { }
    }
    @Test fun preservesJpegsActualPtsAndCoverageWithoutDecoding() = run { dir ->
        val source = File(dir, "source.nvm"); cache(source)
        val pack = File(dir, "pack.nvp"); val restored = File(dir, "restored.nvm")
        val info = ThumbnailPack.export(source, pack)
        assertEquals(info, ThumbnailPack.import(pack, restored))
        assertArrayEquals(source.readBytes(), restored.readBytes())
        assertEquals(3, info.tiles); assertEquals(7, info.slots)
        assertEquals(source.length() + 44, pack.length())
    }
    @Test fun incompleteOrDuplicateRecordsCannotBePublished() = run { dir ->
        val source = File(dir, "source.nvm")
        cache(source, complete = 0)
        rejects { ThumbnailPack.export(source, File(dir, "pack.nvp")) }
        cache(source, keys = listOf(0, 0))
        rejects { ThumbnailPack.export(source, File(dir, "pack.nvp")) }
        assertFalse(File(dir, "pack.nvp").exists())
    }
    @Test fun corruptionCannotReplaceExistingDestination() = run { dir ->
        val source = File(dir, "source.nvm"); cache(source)
        val pack = File(dir, "pack.nvp"); ThumbnailPack.export(source, pack)
        val bytes = pack.readBytes(); bytes[20] = (bytes[20].toInt() xor 1).toByte(); pack.writeBytes(bytes)
        val destination = File(dir, "existing.nvm").apply { writeText("preserve me") }
        rejects { ThumbnailPack.import(pack, destination) }
        assertEquals("preserve me", destination.readText())
    }
    @Test fun truncatedAndOversizedEnvelopesAreRejected() = run { dir ->
        val source = File(dir, "source.nvm"); cache(source)
        val pack = File(dir, "pack.nvp"); ThumbnailPack.export(source, pack)
        pack.writeBytes(pack.readBytes().dropLast(1).toByteArray())
        rejects { ThumbnailPack.import(pack, File(dir, "restored.nvm")) }
        RandomAccessFile(pack, "rw").use { it.seek(4); it.writeLong(Long.MAX_VALUE) }
        rejects { ThumbnailPack.import(pack, File(dir, "restored.nvm")) }
    }
    @Test fun partialRecordHeaderIsRejected() = run { dir ->
        val source = File(dir, "source.nvm"); cache(source)
        source.appendBytes(byteArrayOf(0))
        rejects { ThumbnailPack.inspect(source) }
    }
}

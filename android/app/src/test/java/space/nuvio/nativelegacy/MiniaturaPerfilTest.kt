package space.nuvio.nativelegacy

import org.junit.Assert.assertEquals
import org.junit.Test

class MiniaturaPerfilTest {
    @Test fun lowMemoryDevicesUseSmallerOutput() {
        assertEquals(320, miniaturaLargura(true, 4L shl 30, 0))
        assertEquals(320, miniaturaLargura(false, 2L shl 30, 0))
        assertEquals(320, miniaturaLargura(false, 1L shl 30, 0))
    }
    @Test fun unknownAndLargerDevicesKeepExistingQuality() {
        assertEquals(480, miniaturaLargura(false, 0, 0))
        assertEquals(480, miniaturaLargura(false, 3L shl 30, 0))
    }
    @Test fun diagnosticOverridesPermitComparisonsOnTheSameDevice() {
        assertEquals(480, miniaturaLargura(true, 1L shl 30, 128))
        assertEquals(320, miniaturaLargura(false, 4L shl 30, 64))
        assertEquals(480, miniaturaLargura(true, 1L shl 30, 64 or 128))
    }
}

/*
 * Spektrafilm for Android — unit tests for the retained-result grade cache. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Proves the cache contract: store/lookup round-trip with an EAGER pristine copy
 * (mutating the source buffer after store, or a scratch copy after lookup, never
 * corrupts the retained master), key discrimination on engine params / decode key /
 * edge, single-slot replacement, and clear().
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.SpektraParams
import com.spectrafilm.libraw.WhiteBalance
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class GradeCacheTest {

    private val W = 4
    private val H = 3

    private fun buf(seed: Float): ByteBuffer {
        val b = ByteBuffer.allocateDirect(W * H * 3 * 4).order(ByteOrder.nativeOrder())
        val f = b.asFloatBuffer()
        for (i in 0 until W * H * 3) f.put(i, seed + i * 0.25f)
        return b
    }

    private fun params(film: String = "kodak_portra_400") =
        SpektraParams(filmProfile = film, printProfile = "kodak_portra_endura")

    private fun decodeKey(edge: Int = 640, rotation: Int = 0) = GradeCache.DecodeKey(
        uri = "content://photo/1", kind = "RAW", whiteBalance = WhiteBalance.AS_SHOT,
        temperature = 5500f, tint = 0f, creativeTemp = 0f, creativeTint = 0f,
        filmBalance = "", rotationDegrees = rotation, maxEdge = edge,
    )

    private fun key(film: String = "kodak_portra_400", edge: Int = 640, rotation: Int = 0) =
        GradeCache.Key(engineParams = params(film), decode = decodeKey(edge, rotation))

    @Test
    fun storeLookup_roundTripsDimsAndContent() {
        val cache = GradeCache()
        cache.store(key(), buf(1f), W, H, ColorSpace.SRGB)
        val hit = cache.lookup(key())
        assertNotNull(hit)
        assertEquals(W, hit!!.width)
        assertEquals(H, hit.height)
        assertEquals(ColorSpace.SRGB, hit.colorSpace)
        val f = hit.scratchCopy().asFloatBuffer()
        for (i in 0 until W * H * 3) assertEquals(1f + i * 0.25f, f.get(i), 0f)
    }

    @Test
    fun store_copiesEagerly_sourceMutationAfterStoreIsInvisible() {
        val cache = GradeCache()
        val src = buf(1f)
        cache.store(key(), src, W, H, ColorSpace.SRGB)
        src.asFloatBuffer().put(0, 999f)  // simulate the in-place grade mutating res.data
        val f = cache.lookup(key())!!.scratchCopy().asFloatBuffer()
        assertEquals(1f, f.get(0), 0f)
    }

    @Test
    fun scratchCopy_isIndependent_masterStaysPristine() {
        val cache = GradeCache()
        cache.store(key(), buf(1f), W, H, ColorSpace.SRGB)
        val pristine = cache.lookup(key())!!
        pristine.scratchCopy().asFloatBuffer().put(0, 999f)  // grade pass on a scratch
        assertEquals(1f, pristine.scratchCopy().asFloatBuffer().get(0), 0f)
    }

    @Test
    fun lookup_missesOnAnyEngineOrDecodeChange() {
        val cache = GradeCache()
        cache.store(key(), buf(1f), W, H, ColorSpace.SRGB)
        assertNull("engine param change must miss", cache.lookup(key(film = "kodak_ektar_100")))
        assertNull("edge change must miss", cache.lookup(key(edge = 1024)))
        assertNull("rotation change must miss", cache.lookup(key(rotation = 90)))
        assertNotNull("identical key must still hit", cache.lookup(key()))
    }

    @Test
    fun store_replacesTheSingleSlot() {
        val cache = GradeCache()
        cache.store(key(), buf(1f), W, H, ColorSpace.SRGB)
        cache.store(key(film = "kodak_ektar_100"), buf(2f), W, H, ColorSpace.SRGB)
        assertNull(cache.lookup(key()))
        assertNotNull(cache.lookup(key(film = "kodak_ektar_100")))
    }

    @Test
    fun clear_dropsTheEntry() {
        val cache = GradeCache()
        cache.store(key(), buf(1f), W, H, ColorSpace.SRGB)
        cache.clear()
        assertNull(cache.lookup(key()))
    }
}

/*
 * Spektrafilm for Android — unit tests for source-image rotation. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Proves the pure geometry of Rotation.kt on a tiny LinearImage: rotated() 90/180/270
 * pixel mappings + dimension swaps, flippedHorizontal(), a 4×90° round-trip, and the
 * SourceRotation enum algebra (next/then/fromDegrees). ExifInterface mapping and the
 * >2 GB allocation guard are out of scope (the former needs android.jar, the latter a
 * huge allocation).
 */
package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Test

class RotationTest {

    private val W = 3
    private val H = 2

    /**
     * A [W]×[H] image where every channel encodes its source coordinate:
     * r = x, g = y, b = linear pixel index. Any misplaced pixel is detectable.
     */
    private fun coordImage(w: Int = W, h: Int = H): LinearImage {
        val b = ByteBuffer.allocateDirect(w * h * 3 * 4).order(ByteOrder.nativeOrder())
        val f = b.asFloatBuffer()
        for (y in 0 until h) {
            for (x in 0 until w) {
                val k = (y * w + x) * 3
                f.put(k, x.toFloat())
                f.put(k + 1, y.toFloat())
                f.put(k + 2, (y * w + x).toFloat())
            }
        }
        return LinearImage(b, w, h)
    }

    private fun chan(img: LinearImage, x: Int, y: Int, c: Int): Float =
        img.data.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
            .get((y * img.width + x) * 3 + c)

    /** Assert the pixel at ([x],[y]) of [img] came from source coordinate ([sx],[sy]). */
    private fun assertFrom(img: LinearImage, x: Int, y: Int, sx: Int, sy: Int) {
        assertEquals("r (src x) at ($x,$y)", sx.toFloat(), chan(img, x, y, 0), 0f)
        assertEquals("g (src y) at ($x,$y)", sy.toFloat(), chan(img, x, y, 1), 0f)
        assertEquals("b (src idx) at ($x,$y)", (sy * W + sx).toFloat(), chan(img, x, y, 2), 0f)
    }

    // --- rotated() ---

    @Test
    fun rotateNone_returnsSameInstanceNoCopy() {
        val img = coordImage()
        assertSame(img, img.rotated(SourceRotation.NONE))
    }

    @Test
    fun rotate90_swapsDimensionsAndMapsPixels() {
        val out = coordImage().rotated(SourceRotation.CW90)
        assertEquals(H, out.width)
        assertEquals(W, out.height)
        // CW90: source (x,y) lands at (H-1-y, x).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, H - 1 - y, x, x, y)
        }
    }

    @Test
    fun rotate180_keepsDimensionsAndMapsPixels() {
        val out = coordImage().rotated(SourceRotation.CW180)
        assertEquals(W, out.width)
        assertEquals(H, out.height)
        // CW180: source (x,y) lands at (W-1-x, H-1-y).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, W - 1 - x, H - 1 - y, x, y)
        }
    }

    @Test
    fun rotate270_swapsDimensionsAndMapsPixels() {
        val out = coordImage().rotated(SourceRotation.CW270)
        assertEquals(H, out.width)
        assertEquals(W, out.height)
        // CW270: source (x,y) lands at (y, W-1-x).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, y, W - 1 - x, x, y)
        }
    }

    @Test
    fun fourQuarterTurns_roundTripToOriginal() {
        var img = coordImage()
        repeat(4) { img = img.rotated(SourceRotation.CW90) }
        assertEquals(W, img.width)
        assertEquals(H, img.height)
        for (y in 0 until H) for (x in 0 until W) assertFrom(img, x, y, x, y)
    }

    // --- flippedHorizontal() ---

    @Test
    fun flipHorizontal_mirrorsLeftToRight() {
        val out = coordImage().flippedHorizontal()
        assertEquals(W, out.width)
        assertEquals(H, out.height)
        // Flip H: source (x,y) lands at (W-1-x, y).
        for (y in 0 until H) for (x in 0 until W) {
            assertFrom(out, W - 1 - x, y, x, y)
        }
    }

    @Test
    fun flipHorizontal_twice_isIdentity() {
        val out = coordImage().flippedHorizontal().flippedHorizontal()
        for (y in 0 until H) for (x in 0 until W) assertFrom(out, x, y, x, y)
    }

    // --- SourceRotation algebra ---

    @Test
    fun next_cyclesThroughQuarterTurns() {
        assertEquals(SourceRotation.CW90, SourceRotation.NONE.next())
        assertEquals(SourceRotation.CW180, SourceRotation.CW90.next())
        assertEquals(SourceRotation.CW270, SourceRotation.CW180.next())
        assertEquals(SourceRotation.NONE, SourceRotation.CW270.next())
    }

    @Test
    fun then_composesModulo360() {
        assertEquals(SourceRotation.NONE, SourceRotation.CW90.then(SourceRotation.CW270))
        assertEquals(SourceRotation.NONE, SourceRotation.CW180.then(SourceRotation.CW180))
        assertEquals(SourceRotation.CW270, SourceRotation.CW90.then(SourceRotation.CW180))
        assertEquals(SourceRotation.CW90, SourceRotation.CW90.then(SourceRotation.NONE))
    }

    @Test
    fun fromDegrees_normalizesAnyMultipleOf90() {
        assertEquals(SourceRotation.NONE, SourceRotation.fromDegrees(0))
        assertEquals(SourceRotation.CW90, SourceRotation.fromDegrees(90))
        assertEquals(SourceRotation.CW180, SourceRotation.fromDegrees(180))
        assertEquals(SourceRotation.CW270, SourceRotation.fromDegrees(270))
        assertEquals(SourceRotation.NONE, SourceRotation.fromDegrees(360))
        assertEquals(SourceRotation.CW90, SourceRotation.fromDegrees(450))
        assertEquals(SourceRotation.CW270, SourceRotation.fromDegrees(-90))
        assertEquals(SourceRotation.CW270, SourceRotation.fromDegrees(-450))
    }
}

package com.example.androidpendisplay

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Draws the decoder's output onto the view's surface through a Catmull-Rom
 * bicubic shader instead of letting the compositor stretch it bilinearly.
 *
 * The PC stream is usually smaller than the tablet panel (FHD on a 2944x1840
 * screen is a 1.53x non-integer upscale), and bilinear turns text and thin
 * pen strokes to mush. Doing the upscale here costs one GPU pass on the
 * tablet and adds no bytes to the link and no encode work on the PC.
 *
 * Frame path: MediaCodec -> [inputSurface] (SurfaceTexture) -> draw on the
 * frame callback -> swap onto [outSurface]. No queueing of our own, so a
 * frame is drawn the moment the decoder releases it.
 */
class UpscaleRenderer(private val outSurface: Surface) {
    private val thread = HandlerThread("upscale-gl").apply { start() }
    private val handler = Handler(thread.looper)

    /** Hand this to MediaCodec.configure(). Valid after [start] returns true. */
    @Volatile var inputSurface: Surface? = null
        private set

    @Volatile private var videoW = 1920
    @Volatile private var videoH = 1080

    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var texture: SurfaceTexture? = null
    private var program = 0
    private var uTexMatrix = 0
    private var uSize = 0
    private val texMatrix = FloatArray(16)
    private val quad = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
        put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)).position(0)
    }

    /** Blocks until the GL side is up. False means "use the plain surface instead". */
    fun start(): Boolean {
        val done = CountDownLatch(1)
        var ok = false
        handler.post {
            ok = try { initGl() } catch (e: Exception) { Log.e(TAG, "GL init failed", e); false }
            done.countDown()
        }
        if (!done.await(2, TimeUnit.SECONDS)) return false
        if (!ok) release()
        return ok
    }

    fun setVideoSize(width: Int, height: Int) {
        if (width > 0 && height > 0) { videoW = width; videoH = height }
    }

    fun release() {
        handler.post {
            texture?.release(); texture = null
            inputSurface?.release(); inputSurface = null
            if (display != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, eglSurface)
                if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
                EGL14.eglTerminate(display)
            }
            display = EGL14.EGL_NO_DISPLAY
            thread.quitSafely()
        }
    }

    private fun initGl(): Boolean {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) return false

        val config = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        EGL14.eglChooseConfig(
            display,
            intArrayOf(
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_NONE,
            ), 0, config, 0, 1, count, 0,
        )
        if (count[0] == 0) return false

        context = EGL14.eglCreateContext(
            display, config[0], EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
        )
        eglSurface = EGL14.eglCreateWindowSurface(display, config[0], outSurface, intArrayOf(EGL14.EGL_NONE), 0)
        if (context == EGL14.EGL_NO_CONTEXT || eglSurface == EGL14.EGL_NO_SURFACE) return false
        if (!EGL14.eglMakeCurrent(display, eglSurface, eglSurface, context)) return false
        EGL14.eglSwapInterval(display, 0)  // present as soon as it's drawn

        program = buildProgram() ?: return false
        uTexMatrix = GLES20.glGetUniformLocation(program, "uTexMatrix")
        uSize = GLES20.glGetUniformLocation(program, "uSize")

        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex[0])
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        val st = SurfaceTexture(tex[0])
        st.setOnFrameAvailableListener({ drawFrame() }, handler)
        texture = st
        inputSurface = Surface(st)
        return true
    }

    private fun drawFrame() {
        val st = texture ?: return
        try {
            st.updateTexImage()
            st.getTransformMatrix(texMatrix)
            val w = IntArray(1)
            val h = IntArray(1)
            EGL14.eglQuerySurface(display, eglSurface, EGL14.EGL_WIDTH, w, 0)
            EGL14.eglQuerySurface(display, eglSurface, EGL14.EGL_HEIGHT, h, 0)
            GLES20.glViewport(0, 0, w[0], h[0])
            GLES20.glUseProgram(program)
            GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0)
            GLES20.glUniform2f(uSize, videoW.toFloat(), videoH.toFloat())
            GLES20.glEnableVertexAttribArray(0)
            GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, quad)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            EGL14.eglSwapBuffers(display, eglSurface)
        } catch (e: Exception) {
            Log.e(TAG, "draw failed", e)
        }
    }

    private fun buildProgram(): Int? {
        val vs = compile(GLES20.GL_VERTEX_SHADER, VERTEX) ?: return null
        val fs = compile(GLES20.GL_FRAGMENT_SHADER, FRAGMENT) ?: return null
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs)
        GLES20.glAttachShader(p, fs)
        GLES20.glBindAttribLocation(p, 0, "aPos")
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        if (ok[0] == 0) { Log.e(TAG, "link: " + GLES20.glGetProgramInfoLog(p)); return null }
        return p
    }

    private fun compile(type: Int, src: String): Int? {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) { Log.e(TAG, "shader: " + GLES20.glGetShaderInfoLog(s)); return null }
        return s
    }

    companion object {
        private const val TAG = "UpscaleRenderer"

        private const val VERTEX = """
            attribute vec2 aPos;
            uniform mat4 uTexMatrix;
            varying vec2 vUv;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                vUv = (uTexMatrix * vec4(aPos * 0.5 + 0.5, 0.0, 1.0)).xy;
            }
        """

        // Catmull-Rom bicubic in 9 bilinear taps (the usual "filmic" trick:
        // the two middle taps are merged into one fetch between them).
        private const val FRAGMENT = """
            #extension GL_OES_EGL_image_external : require
            precision highp float;
            uniform samplerExternalOES sTex;
            uniform vec2 uSize;
            varying vec2 vUv;
            void main() {
                vec2 tc = vUv * uSize;
                vec2 c = floor(tc - 0.5) + 0.5;
                vec2 f = tc - c;
                vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
                vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
                vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
                vec2 w3 = f * f * (-0.5 + 0.5 * f);
                vec2 w12 = w1 + w2;
                vec2 inv = 1.0 / uSize;
                vec2 t0 = (c - 1.0) * inv;
                vec2 t12 = (c + w2 / w12) * inv;
                vec2 t3 = (c + 2.0) * inv;
                vec4 r =
                    texture2D(sTex, vec2(t0.x, t0.y)) * (w0.x * w0.y) +
                    texture2D(sTex, vec2(t12.x, t0.y)) * (w12.x * w0.y) +
                    texture2D(sTex, vec2(t3.x, t0.y)) * (w3.x * w0.y) +
                    texture2D(sTex, vec2(t0.x, t12.y)) * (w0.x * w12.y) +
                    texture2D(sTex, vec2(t12.x, t12.y)) * (w12.x * w12.y) +
                    texture2D(sTex, vec2(t3.x, t12.y)) * (w3.x * w12.y) +
                    texture2D(sTex, vec2(t0.x, t3.y)) * (w0.x * w3.y) +
                    texture2D(sTex, vec2(t12.x, t3.y)) * (w12.x * w3.y) +
                    texture2D(sTex, vec2(t3.x, t3.y)) * (w3.x * w3.y);
                gl_FragColor = vec4(clamp(r.rgb, 0.0, 1.0), 1.0);
            }
        """
    }
}

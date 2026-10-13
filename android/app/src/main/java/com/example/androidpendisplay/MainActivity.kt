// File: app/src/main/java/com/example/androidpendisplay/MainActivity.kt
package com.example.androidpendisplay

import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.text.InputType
import android.util.Log
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.PopupWindow
import android.widget.Toast
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : AppCompatActivity() {
    private var decoder: H264Decoder? = null
    private var decoderPipeline: DecoderPipeline? = null
    private var upscaler: UpscaleRenderer? = null
    private var udpReceiver: UdpReceiver? = null
    private var videoReassembler: VideoReassembler? = null
    private var penSender: PenInputSender? = null
    private var controlChannel: ControlChannel? = null
    private var statusPopup: PopupWindow? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var wakeLock: PowerManager.WakeLock? = null
    @Volatile private var lastStats: VideoReassembler.Stats? = null
    @Volatile private var lastDecodeMsX10 = 0
    private lateinit var penSurfaceView: PenSurfaceView
    private lateinit var inkOverlayView: InkOverlayView
    private lateinit var statusPill: TextView
    private lateinit var statusDetail: TextView
    private lateinit var changeIpButton: Button
    private lateinit var openTetheringButton: Button
    private var localIp = "Unavailable"
    private var detailExpanded = false
    private var lastCompletedFrames = 0L

    private val statsHandler = Handler(Looper.getMainLooper())
    private val statsRunnable = object : Runnable {
        override fun run() {
            updateStatus()
            statsHandler.postDelayed(this, 1000L)
        }
    }
    private val isInitialized = AtomicBoolean(false)
    private val TAG = "MainActivity"

    // PC IP: loaded from SharedPreferences, editable at runtime. Falls back to
    // NetworkUtils' default the first time the app runs.
    private var pcIp: String = NetworkUtils.DEFAULT_PC_IP
    private val videoPort = 5000
    private val inputPort = 5001

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                        View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                        View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                )

        penSurfaceView = findViewById(R.id.penSurfaceView)
        inkOverlayView = findViewById(R.id.inkOverlayView)
        penSurfaceView.setInkOverlay(inkOverlayView)

        // The status panel lives in its own window. Both SurfaceViews use
        // setZOrderOnTop(true), so anything in the activity layout is drawn
        // UNDER the video; a PopupWindow composites above it.
        val panel = layoutInflater.inflate(R.layout.status_panel, null)
        statusPill = panel.findViewById(R.id.statusPill)
        statusDetail = panel.findViewById(R.id.statusDetail)
        changeIpButton = panel.findViewById(R.id.changeIpButton)
        openTetheringButton = panel.findViewById(R.id.openTetheringButton)
        val popup = PopupWindow(
            panel,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            false,  // not focusable: the pen must keep getting touch events
        )
        statusPopup = popup
        val margin = (12 * resources.displayMetrics.density).toInt()
        // Needs a window token, so not before the activity's own window exists.
        penSurfaceView.post {
            if (!isFinishing && !isDestroyed) {
                popup.showAtLocation(penSurfaceView, Gravity.TOP or Gravity.START, margin, margin)
            }
        }

        // Tap the pill to fold the detailed stats in/out, so the overlay stays
        // out of the way while drawing but the numbers are one tap away.
        statusPill.setOnClickListener { setDetailExpanded(!detailExpanded) }
        changeIpButton.setOnClickListener { showEditPcIpDialog() }
        openTetheringButton.setOnClickListener { openTetheringSettings() }
        setDetailExpanded(false)

        if (!NetworkUtils.isNetworkAvailable(this)) {
            Toast.makeText(this, R.string.toast_no_network, Toast.LENGTH_LONG).show()
        }

        localIp = NetworkUtils.getLocalIpAddress()
        pcIp = loadSavedPcIp()
        updateStatus()
        Log.d(TAG, "Android IP: $localIp, PC IP: $pcIp")

        penSurfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.d(TAG, "Surface created")
                initialize()
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.d(TAG, "Surface changed: ${width}x${height}")
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.d(TAG, "Surface destroyed")
                release()
            }
        })
    }

    private fun setDetailExpanded(expanded: Boolean) {
        detailExpanded = expanded
        val visibility = if (expanded) View.VISIBLE else View.GONE
        statusDetail.visibility = visibility
        changeIpButton.visibility = visibility
        openTetheringButton.visibility = visibility
        // The popup measured itself when it was shown; re-measure it so it
        // grows and shrinks with the detail section.
        statusPopup?.update()
    }

    /**
     * Jumps straight to the Tethering settings screen so the user doesn't
     * have to dig through Settings > Network to flip on USB tethering.
     * android.settings.TETHER_SETTINGS is an unofficial but widely supported
     * action across stock/AOSP-based ROMs; fall back to the general wireless
     * settings screen on OEM builds that don't expose it directly.
     */
    private fun openTetheringSettings() {
        try {
            startActivity(Intent("android.settings.TETHER_SETTINGS"))
        } catch (e: ActivityNotFoundException) {
            try {
                startActivity(Intent(android.provider.Settings.ACTION_WIRELESS_SETTINGS))
            } catch (e2: ActivityNotFoundException) {
                showToast(getString(R.string.toast_tethering_settings_unavailable))
            }
        }
    }

    /**
     * The once-a-second report the PC's QoS runs on. Called from the control
     * channel's thread; every counter it reads is drained here so the window
     * matches the report interval.
     */
    private fun buildStatus(): ControlChannel.ClientStatus {
        val stats = videoReassembler?.consumeStats()
        val decodeMsX10 = decoderPipeline?.consumeDecodeMsX10() ?: 0
        lastStats = stats
        lastDecodeMsX10 = decodeMsX10
        return ControlChannel.ClientStatus(
            decodeQueueLength = decoderPipeline?.queueLength() ?: 0,
            lossPermille = stats?.lossPermille ?: 0,
            completedFps = stats?.completedFps ?: 0,
            droppedFrames = (stats?.droppedFrames ?: 0) + (decoderPipeline?.consumeDropped() ?: 0),
            decodeMsX10 = decodeMsX10,
        )
    }

    /**
     * Wi-Fi power saving parks the radio between packets, which shows up as
     * tens of milliseconds of jitter on exactly the traffic this app cares
     * about. The wake lock keeps the CPU from sleeping mid-stroke; the
     * activity's keepScreenOn covers the display.
     */
    private fun acquireLocks() {
        try {
            val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            // LOW_LATENCY (API 29+) is the mode built for exactly this: a
            // foreground app with the screen on streaming in real time. It
            // disables power save *and* asks the radio for low-latency
            // scheduling. HIGH_PERF is deprecated and no longer does
            // anything on current releases, so it's only the pre-Q fallback.
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                @Suppress("DEPRECATION")
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifiLock = wifiManager.createWifiLock(mode, "DisplayTwin::Wifi").apply {
                setReferenceCounted(false)
                acquire()
            }
            val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = powerManager.newWakeLock(
                PowerManager.PARTIAL_WAKE_LOCK, "DisplayTwin::Wake"
            ).apply {
                setReferenceCounted(false)
                acquire(WAKE_LOCK_TIMEOUT_MS)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Could not acquire wifi/wake locks", e)
        }
    }

    private fun releaseLocks() {
        try {
            wifiLock?.takeIf { it.isHeld }?.release()
            wakeLock?.takeIf { it.isHeld }?.release()
        } catch (e: Exception) {
            Log.w(TAG, "Lock release failed", e)
        }
        wifiLock = null
        wakeLock = null
    }

    private fun updateStatus() {
        val completed = videoReassembler?.completedFrames() ?: 0L
        val fps = (completed - lastCompletedFrames).coerceAtLeast(0L)
        lastCompletedFrames = completed
        val connected = isInitialized.get() && fps > 0

        statusPill.text = if (connected) {
            "●  PC $pcIp   ${fps}fps"
        } else {
            "○  PC $pcIp   ${getString(R.string.status_waiting)}"
        }
        statusPill.setTextColor(if (connected) 0xFF7CE38B.toInt() else 0xFFFFC44D.toInt())

        // Now that the round trip is measured, the local ink prediction can
        // hold its ghost stroke for as long as the real ink actually takes
        // to come back instead of a fixed guess.
        controlChannel?.let { channel ->
            if (channel.rttMs > 0) {
                inkOverlayView.setRoundTripMs(channel.rttMs, lastDecodeMsX10 / 10.0f)
            }
        }

        if (detailExpanded) {
            val channel = controlChannel
            val stats = lastStats
            statusDetail.text = buildString {
                append("Android IP : $localIp\n")
                append("PC IP      : $pcIp\n")
                append("Ports      : video $videoPort / pen $inputPort / control 5002\n")
                append("Session    : ")
                append(
                    when {
                        channel == null -> "off"
                        channel.sessionId != 0 -> "0x%08x".format(channel.sessionId)
                        else -> "handshaking"
                    }
                )
                append("   RTT ")
                append(if (channel != null && channel.rttMs > 0) "${channel.rttMs}ms" else "-")
                append(if (channel?.hasClockOffset == true) "  clock synced\n" else "  clock -\n")
                append("Loss       : ")
                append(if (stats != null) "%.1f%%".format(stats.lossPermille / 10.0) else "-")
                append("   FEC recovered ${stats?.recoveredFragments ?: 0}")
                append("   PLI ${channel?.pliCount() ?: 0}\n")
                append("Decoder    : queue ${decoderPipeline?.queueLength() ?: 0}")
                append("   ${"%.1f".format(lastDecodeMsX10 / 10.0)}ms")
                append("   dropped ${decoderPipeline?.droppedFrames() ?: 0}\n")
                append("Pen        : retransmits ${penSender?.retransmitCount() ?: 0}\n")
                append("${videoReassembler?.stats() ?: "UDP=0"}\n")
                append(decoder?.stats() ?: "decoder=none")
            }
        }
    }

    private fun loadSavedPcIp(): String {
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return prefs.getString(KEY_PC_IP, null) ?: NetworkUtils.DEFAULT_PC_IP
    }

    private fun savePcIp(ip: String) {
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_PC_IP, ip)
            .apply()
    }

    private fun isValidIpv4(ip: String): Boolean {
        val parts = ip.trim().split(".")
        if (parts.size != 4) return false
        return parts.all { part -> part.toIntOrNull()?.let { it in 0..255 } == true }
    }

    private fun showEditPcIpDialog() {
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText(pcIp)
            setSelection(text.length)
        }
        val scanButton = Button(this).apply { setText(R.string.dialog_scan_qr) }
        val content = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            addView(input)
            addView(scanButton)
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.dialog_pc_ip_title)
            .setMessage(R.string.dialog_pc_ip_message)
            .setView(content)
            .setPositiveButton(R.string.dialog_save) { _, _ ->
                val newIp = input.text.toString().trim()
                if (!isValidIpv4(newIp)) {
                    showToast(getString(R.string.toast_invalid_ip))
                    return@setPositiveButton
                }
                applyPcIp(newIp)
            }
            .setNegativeButton(R.string.dialog_cancel, null)
            // Beats reading an IP off the PC screen and typing it in: the
            // host answers a broadcast on UDP 9999 with its name.
            .setNeutralButton(R.string.dialog_discover) { _, _ -> discoverHosts() }
            .show()
        scanButton.setOnClickListener {
            dialog.dismiss()
            scanQr()
        }
    }

    private fun applyPcIp(ip: String) {
        if (ip == pcIp) return
        pcIp = ip
        savePcIp(ip)
        showToast(getString(R.string.toast_ip_saved, ip))
        // Reconnect with the new address.
        release()
        initialize()
    }

    /** The PC shows `dtwin://<ip>?mode=usb|wifi`; the host is the PC address. */
    private fun scanQr() {
        com.google.mlkit.vision.codescanner.GmsBarcodeScanning.getClient(this).startScan()
            .addOnSuccessListener { barcode ->
                val uri = android.net.Uri.parse(barcode.rawValue.orEmpty())
                val ip = uri.host
                if (uri.scheme != "dtwin" || ip == null || !isValidIpv4(ip)) {
                    showToast(getString(R.string.toast_qr_invalid))
                } else {
                    applyPcIp(ip)
                }
            }
            .addOnFailureListener { showToast(getString(R.string.toast_qr_failed)) }
    }

    private fun discoverHosts() {
        showToast(getString(R.string.toast_searching))
        Thread {
            val hosts = Discovery.search()
            runOnUiThread {
                if (hosts.isEmpty()) {
                    showToast(getString(R.string.toast_no_pc_found))
                    return@runOnUiThread
                }
                val labels = hosts.map { "${it.name} (${it.ip})" }.toTypedArray()
                AlertDialog.Builder(this)
                    .setTitle(R.string.dialog_pick_pc_title)
                    .setItems(labels) { _, index ->
                        applyPcIp(hosts[index].ip)
                    }
                    .setNegativeButton(R.string.dialog_cancel, null)
                    .show()
            }
        }.start()
    }

    private fun initialize() {
        if (isInitialized.getAndSet(true)) {
            Log.d(TAG, "Already initialized")
            return
        }

        Log.d(TAG, "Initializing components...")

        acquireLocks()

        // 1. Pen sender (connect() runs on its own background thread). It
        //    stamps packets with the session the control channel negotiates
        //    and paces its retransmits off the measured round trip.
        penSender = PenInputSender(
            ip = pcIp,
            port = inputPort,
            sessionIdProvider = { controlChannel?.sessionId ?: 0 },
            rttMsProvider = { controlChannel?.rttMs ?: 0 },
        ).apply {
            connect { success ->
                if (!success) {
                    Log.e(TAG, "PenInputSender connect failed")
                    showToast(getString(R.string.toast_pen_connect_failed))
                }
            }
        }

        // 2. Decoder - once the real stream size is known, resize the surface
        //    so the picture keeps the PC's aspect ratio and touch mapping
        //    lines up with what's displayed.
        // Bicubic upscale on the tablet instead of the compositor's bilinear
        // stretch; falls back to the plain surface if GL setup fails.
        upscaler = UpscaleRenderer(penSurfaceView.holder.surface).takeIf { it.start() }
        decoder = H264Decoder(upscaler?.inputSurface ?: penSurfaceView.holder.surface) { width, height ->
            upscaler?.setVideoSize(width, height)
            runOnUiThread {
                penSurfaceView.setVideoSize(width, height)
                inkOverlayView.setVideoSize(width, height)
            }
        }
        if (decoder?.init() != true) {
            Log.e(TAG, "Decoder init failed")
            showToast(getString(R.string.toast_decoder_failed))
            isInitialized.set(false)
            return
        }

        // 3. Control channel first, so the session/clock are being
        //    negotiated while the video path comes up.
        val channel = ControlChannel(
            pcIp = pcIp,
            statusProvider = { buildStatus() },
            onSession = { id ->
                Log.d(TAG, "Session established: $id")
                videoReassembler?.reset()
            },
            onBusy = { showToast(getString(R.string.toast_pc_busy)) },
        )
        channel.onPenAck = { sequence -> penSender?.onAck(sequence) }
        controlChannel = channel
        channel.start()

        // 4. Decode pipeline: its own thread, a short queue, a keyframe gate
        //    and a stale-frame policy. Decoding used to run inline on the
        //    receive thread, where a slow frame stalled the socket.
        decoderPipeline = DecoderPipeline(
            decoder = decoder!!,
            onNeedKeyframe = { controlChannel?.sendPli() },
            pcClockMs = { controlChannel?.takeIf { it.hasClockOffset }?.pcNowMs() },
        ).also { it.start() }

        // 5. Receive + reassemble (with FEC recovery).
        videoReassembler = VideoReassembler(
            onFrame = { data, timestampMs, frameId ->
                decoderPipeline?.submit(data, timestampMs, frameId)
            },
            onFrameLost = { controlChannel?.sendPli() },
            expectedSessionId = { controlChannel?.sessionId ?: 0 },
        )
        udpReceiver = UdpReceiver(videoPort, pcIp) { data, length ->
            videoReassembler?.accept(data, length)
        }

        if (udpReceiver?.start() != true) {
            Log.e(TAG, "UDP receiver start failed")
            showToast(getString(R.string.toast_receiver_failed))
            isInitialized.set(false)
            return
        }

        lastCompletedFrames = 0L
        statsHandler.removeCallbacks(statsRunnable)
        statsHandler.post(statsRunnable)

        penSender?.let { penSurfaceView.setPenSender(it) }
        penSurfaceView.touchSink = { contacts -> controlChannel?.sendTouch(contacts) }

        updateStatus()
        Log.d(TAG, "Initialization complete")
    }

    private fun release() {
        if (!isInitialized.getAndSet(false)) {
            return
        }

        Log.d(TAG, "Releasing components...")
        statsHandler.removeCallbacks(statsRunnable)
        inkOverlayView.onPenCancel()
        // Stop accepting completed frames before releasing MediaCodec.
        videoReassembler = null
        udpReceiver?.stop()
        udpReceiver = null
        controlChannel?.stop()
        controlChannel = null
        decoderPipeline?.stop()
        decoderPipeline = null
        decoder?.release()
        decoder = null
        upscaler?.release()
        upscaler = null
        penSender?.close()
        penSender = null
        lastCompletedFrames = 0L
        lastStats = null
        releaseLocks()
        Log.d(TAG, "Release complete")
    }

    private fun showToast(message: String) {
        runOnUiThread {
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        statusPopup?.dismiss()
        statusPopup = null
        release()
    }

    companion object {
        private const val PREFS_NAME = "display_twin_prefs"
        private const val KEY_PC_IP = "pc_ip"
        // A drawing session is long, but an unbounded PARTIAL_WAKE_LOCK is a
        // battery bug waiting to happen if a release path is ever missed.
        private const val WAKE_LOCK_TIMEOUT_MS = 4 * 60 * 60 * 1000L
    }
}

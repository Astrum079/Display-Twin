#include "app/HostController.h"

#include "app/HostConfig.h"
#include "capture/ScreenCapture.h"
#include "encoder/MediaFoundationEncoder.h"
#include "encoder/NvencEncoder.h"
#include "encoder/VideoEncoder.h"
#include "hid/VirtualPen.h"
#include "network/NetworkInfo.h"
#include "network/UdpTransport.h"
#include "protocol/Control.h"
#include "protocol/Packets.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr const char* kHostVersion = "DisplayTwin-PC/4.2";

// Session/recovery timing, all from the v4 spec.
constexpr uint64_t kSessionTimeoutMs = 3000;
constexpr uint64_t kPliMinIntervalMs = 100;
constexpr uint64_t kKeepaliveIntervalMs = 1000;  // static screen: >= 1fps
constexpr uint64_t kQosIntervalMs = 2000;
// Keyframe cadence when no tablet has completed the handshake. Such a
// client (control port firewalled, or an older app build) has no way to
// send a PLI, so without this it would never recover from a lost fragment.
// With a session there is no periodic IDR at all - see NvencEncoder.
constexpr uint64_t kNoSessionKeyframeIntervalMs = 2000;

// Pen MOVE samples lost in flight leave a straight-line gap in the stroke.
// Filling it in is better than a visible corner, but a long gap means the
// link hiccuped badly and inventing a dozen points would draw a line the
// user never made - so cap how much we are willing to fabricate.
constexpr int kMaxInterpolatedSamples = 8;

// QoS never drops below this; under it the picture is too soft to draw on.
constexpr int kMinBitrateKbps = 4000;

uint64_t SteadyMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string IpToString(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    return buffer;
}

// A tablet is identified by its address, not by the ephemeral port its
// control socket happened to get: relaunching the app gives it a new port,
// and comparing ports would make the reconnect look like a second client
// and get it refused with BUSY.
bool SameHost(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

uint32_t MakeSessionId() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<uint32_t> distribution(1, 0xFFFFFFFEu);
    return distribution(generator);
}

std::string SessionLogPath() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char name[64] = {};
    std::strftime(name, sizeof(name), "session_%Y%m%d_%H%M%S.log", &local);
    return std::string("logs\\") + name;
}

// The tablet's report plus our own encode timing, turned into "should we ask
// for less?". Dual thresholds with hysteresis so a single bad 2-second
// window doesn't cost the user their framerate, and a good window doesn't
// immediately undo a downgrade that is holding the link together.
class QosController {
public:
    QosController(int baseFramerate, int baseBitrateKbps)
        : baseFramerate_(baseFramerate), baseBitrateKbps_(baseBitrateKbps),
          framerate_(baseFramerate), bitrateKbps_(baseBitrateKbps) {}

    struct Decision {
        bool changed = false;
        int framerate = 0;
        int bitrateKbps = 0;
        std::string reason;
    };

    Decision Evaluate(float encodeMs, uint32_t lossPermille, uint32_t decodeQueue) {
        const bool bad = encodeMs > 15.0f || lossPermille > 20u || decodeQueue > 4u;
        const bool good = encodeMs < 8.0f && lossPermille < 5u && decodeQueue < 2u;
        const bool lossHeavy = lossPermille > 30u;

        badStreak_ = bad ? badStreak_ + 1 : 0;
        goodStreak_ = good ? goodStreak_ + 1 : 0;
        lossStreak_ = lossHeavy ? lossStreak_ + 1 : 0;

        Decision decision;
        // Independent bitrate cut: heavy loss for 3 evaluations (6s) means
        // the link can't carry this much data, whatever the framerate.
        if (lossStreak_ >= 3 && bitrateKbps_ > kMinBitrateKbps) {
            lossStreak_ = 0;
            return Apply(framerate_, std::max(kMinBitrateKbps, bitrateKbps_ * 7 / 10),
                         "sustained packet loss");
        }

        if (badStreak_ >= 2) {
            badStreak_ = 0;
            // Degradation priority: bitrate first (least visible), then
            // framerate, then the bitrate floor.
            if (bitrateKbps_ > baseBitrateKbps_ * 7 / 10) {
                return Apply(framerate_, std::max(kMinBitrateKbps, bitrateKbps_ * 7 / 10),
                             "encode/loss/queue over threshold");
            }
            if (framerate_ > 30) {
                return Apply(30, bitrateKbps_, "still over threshold at reduced bitrate");
            }
            if (bitrateKbps_ > kMinBitrateKbps) {
                return Apply(framerate_, kMinBitrateKbps, "still over threshold at 30fps");
            }
            return decision;
        }

        // Recovery needs every metric healthy for 5 evaluations (10s), and
        // climbs back one step at a time - framerate first, since that is
        // what the user notices.
        if (goodStreak_ >= 5 && (framerate_ < baseFramerate_ || bitrateKbps_ < baseBitrateKbps_)) {
            goodStreak_ = 0;
            if (framerate_ < baseFramerate_) {
                return Apply(baseFramerate_, bitrateKbps_, "link healthy - restoring framerate");
            }
            return Apply(framerate_, std::min(baseBitrateKbps_, bitrateKbps_ * 13 / 10),
                         "link healthy - raising bitrate");
        }
        return decision;
    }

    int framerate() const { return framerate_; }
    int bitrateKbps() const { return bitrateKbps_; }

private:
    Decision Apply(int framerate, int bitrateKbps, std::string reason) {
        Decision decision;
        if (framerate == framerate_ && bitrateKbps == bitrateKbps_) return decision;
        framerate_ = framerate;
        bitrateKbps_ = bitrateKbps;
        decision.changed = true;
        decision.framerate = framerate_;
        decision.bitrateKbps = bitrateKbps_;
        decision.reason = std::move(reason);
        return decision;
    }

    int baseFramerate_;
    int baseBitrateKbps_;
    int framerate_;
    int bitrateKbps_;
    int badStreak_ = 0;
    int goodStreak_ = 0;
    int lossStreak_ = 0;
};

void Log(const HostController::LogCallback& callback, const std::string& message) {
    if (callback) callback(message);
}

}  // namespace

HostController::~HostController() { Stop(); }

bool HostController::Start(const HostSettings& settings, LogCallback log) {
    if (running_.exchange(true)) return false;
    worker_ = std::thread(&HostController::Run, this, settings, std::move(log));
    return true;
}

void HostController::Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void HostController::UpdateTabletIp(const std::string& ip) {
    if (!running_.load()) return;
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingTabletIp_ = ip;
    hasPendingTabletIp_ = true;
    pendingPenReset_ = true;
}

void HostController::Run(HostSettings settings, LogCallback rawLog) {
    // Every log line also goes to logs/session_<timestamp>.log so a problem
    // that only shows up after twenty minutes of drawing can be read back
    // instead of reconstructed from memory.
    std::shared_ptr<std::ofstream> logFile;
    if (settings.writeSessionLog) {
        CreateDirectoryA("logs", nullptr);  // ERROR_ALREADY_EXISTS is fine.
        auto file = std::make_shared<std::ofstream>(SessionLogPath(), std::ios::out | std::ios::trunc);
        if (file->is_open()) logFile = file;
    }
    const uint64_t startMs = SteadyMs();
    LogCallback log = [rawLog, logFile, startMs](const std::string& message) {
        if (logFile) {
            *logFile << "[" << (SteadyMs() - startMs) << "ms] " << message << "\n";
            logFile->flush();
        }
        if (rawLog) rawLog(message);
    };

    Log(log, std::string("[PC] Starting host (") + kHostVersion + ")...");
    ScreenCapture capture;
    if (!capture.Initialize()) {
        Log(log, "[ERROR] ScreenCapture initialization failed");
        running_ = false;
        return;
    }

    {
        std::ostringstream caps;
        caps << "[PC] Capturing " << capture.GetWidth() << "x" << capture.GetHeight()
             << " at desktop origin (" << capture.GetDesktopLeft() << "," << capture.GetDesktopTop()
             << ") -> encoding at " << settings.framerate << "fps / "
             << settings.bitrateKbps << "kbps";
        Log(log, caps.str());
    }

    // NVENC when there is an NVIDIA GPU, Windows' own software H.264 encoder
    // otherwise. The software path caps the picture at 720p and costs real
    // latency (see MediaFoundationEncoder) - it is there so the host runs at
    // all on a machine without a discrete GPU.
    std::unique_ptr<IVideoEncoder> encoderPtr(new NvencEncoder(
        capture.GetWidth(), capture.GetHeight(), settings.framerate, settings.bitrateKbps));
    if (!encoderPtr->Initialize(capture.GetD3DDevice())) {
        Log(log, "[WARN] NVENC unavailable - falling back to the software H.264 encoder "
                 "(capped at 720p, expect higher latency)");
        encoderPtr.reset(new MediaFoundationEncoder(
            capture.GetWidth(), capture.GetHeight(), settings.framerate, settings.bitrateKbps));
        if (!encoderPtr->Initialize(capture.GetD3DDevice())) {
            Log(log, "[ERROR] No usable H.264 encoder: NVENC and Media Foundation both failed");
            running_ = false;
            return;
        }
    }
    IVideoEncoder& encoder = *encoderPtr;

    UdpSender videoSender(settings.tabletIp, settings.videoPort);
    if (!videoSender.IsValid()) {
        Log(log, "[ERROR] Video UDP sender initialization failed");
        running_ = false;
        return;
    }

    VirtualPen virtualPen;
    if (!virtualPen.Initialize()) Log(log, "[WARN] VirtualPen initialization failed");
    // Pen coordinates are normalized against the video the tablet shows, so
    // they must land on the rectangle we actually capture - not on whatever
    // GetSystemMetrics considers "the screen".
    virtualPen.SetTargetRect(capture.GetDesktopLeft(), capture.GetDesktopTop(),
                             capture.GetWidth(), capture.GetHeight());

    // ---- Session state, shared with the control-channel thread ----
    std::mutex sessionMutex;
    bool sessionActive = false;
    uint32_t sessionId = 0;
    sockaddr_in sessionAddress{};
    uint64_t sessionLastSeenMs = 0;
    std::atomic<uint64_t> lastKeyframeRequestMs{0};
    std::atomic<uint32_t> pliTotal{0};
    std::atomic<uint32_t> qosDowngradeTotal{0};
    std::atomic<bool> statusFresh{false};
    ClientStatus latestStatus{};
    std::mutex statusMutex;

    UdpEndpoint control(settings.controlPort);
    if (!control.IsValid()) {
        Log(log, "[WARN] Control channel (UDP " + std::to_string(settings.controlPort) +
                     ") could not bind - PLI/clock sync/QoS will be unavailable");
    }

    // ---- Pen input ----
    std::atomic_bool havePenSequence{false};
    std::atomic<uint16_t> lastPenSequence{0};
    // Last accepted sample, used to fill gaps left by lost MOVE packets.
    std::mutex penStateMutex;
    PenInputPacket lastPenPacket{};
    bool havePenPacket = false;

    UdpReceiver inputReceiver(settings.inputPort);
    // Belt and braces: a session handshake now assigns a SessionID, but the
    // source lock still keeps a stray sender from being processed at all in
    // the window before HELLO arrives.
    inputReceiver.EnableSourceLock(true);

    inputReceiver.SetCallback([&](const uint8_t* data, size_t len) {
        PenInputPacket packet;
        if (!ParsePenPacket(data, len, packet) ||
            !std::isfinite(packet.x) || !std::isfinite(packet.y) ||
            !std::isfinite(packet.pressure) || !std::isfinite(packet.tilt_x) ||
            !std::isfinite(packet.tilt_y)) {
            return;
        }

        // Once a session exists, only that session's packets are honored.
        // Before it does (older app builds, or the gap before HELLO) the
        // source lock above is the only gate.
        uint32_t activeId = 0;
        bool haveSession = false;
        sockaddr_in replyTo{};
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            haveSession = sessionActive;
            activeId = sessionId;
            replyTo = sessionAddress;
        }
        if (haveSession && packet.sessionId != activeId) return;

        // Wi-Fi UDP can deliver packets out of order. Android also fans out
        // MotionEvent's historical samples as separate packets right before
        // the current one, which makes reordering more likely to actually
        // happen in a burst. Applying a stale MOVE after a newer one (or
        // after UP) makes the injected pointer visibly jitter/snap backward.
        const bool stale = havePenSequence && !IsSequenceNewer(packet.sequence, lastPenSequence);
        if (stale) {
            // A retransmitted DOWN/UP still needs an ACK, or the tablet will
            // keep resending a state change we already applied.
            if (haveSession && control.IsValid() &&
                (packet.action == kPenActionDown || packet.action == kPenActionUp ||
                 packet.action == kPenActionCancel)) {
                control.SendTo(replyTo, BuildPenAck(activeId, packet.sequence));
            }
            return;
        }

        const uint16_t previousSequence = lastPenSequence.load();
        const bool hadSequence = havePenSequence.load();
        havePenSequence = true;
        lastPenSequence = packet.sequence;

        // Fill in samples lost between the last accepted MOVE and this one.
        if (packet.action == kPenActionMove && hadSequence) {
            const uint16_t gap = static_cast<uint16_t>(packet.sequence - previousSequence);
            std::lock_guard<std::mutex> lock(penStateMutex);
            if (havePenPacket && gap > 1 && (lastPenPacket.buttons & 0x01) != 0 &&
                (packet.buttons & 0x01) != 0) {
                const int missing = std::min<int>(gap - 1, kMaxInterpolatedSamples);
                for (int step = 1; step <= missing; ++step) {
                    const float t = static_cast<float>(step) / static_cast<float>(missing + 1);
                    PenInputPacket filled = packet;
                    filled.x = lastPenPacket.x + (packet.x - lastPenPacket.x) * t;
                    filled.y = lastPenPacket.y + (packet.y - lastPenPacket.y) * t;
                    filled.pressure = lastPenPacket.pressure +
                                      (packet.pressure - lastPenPacket.pressure) * t;
                    virtualPen.InjectInput(filled);
                }
            }
        }

        virtualPen.InjectInput(packet);
        {
            std::lock_guard<std::mutex> lock(penStateMutex);
            lastPenPacket = packet;
            havePenPacket = true;
        }

        // State changes are the samples whose loss is actually visible (a
        // missing DOWN eats the start of a stroke, a missing UP leaves the
        // tip stuck), so they are acknowledged and the tablet retransmits
        // until the ACK arrives. MOVEs are never acknowledged - by the time
        // a retransmit landed, the stroke has moved on.
        if (haveSession && control.IsValid() &&
            (packet.action == kPenActionDown || packet.action == kPenActionUp ||
             packet.action == kPenActionCancel)) {
            control.SendTo(replyTo, BuildPenAck(activeId, packet.sequence));
        }
    });
    if (!inputReceiver.Start()) Log(log, "[WARN] Input UDP receiver failed to start");

    // Ends the current pen session: unlocks the input source (so a packet
    // from a new address is accepted instead of dropped forever), lifts a
    // tip that was mid-drag when the link died, and clears sequence
    // tracking (so the next session's numbering, which may restart from 0
    // if the Android app itself relaunched, isn't rejected as "stale").
    // FEC costs ~12.5% extra datagrams and only pays off on a lossy link.
    // Over USB tethering (the tablet is that adapter's gateway) loss is
    // essentially nil, so skip it there and save the airtime/latency.
    std::atomic<bool> useFec{settings.enableFec};
    auto updateFec = [&](const std::string& tabletIp) {
        bool usb = false;
        for (const AdapterInfo& adapter : EnumerateIPv4Adapters()) {
            if (adapter.likelyTethering && adapter.gateway == tabletIp) { usb = true; break; }
        }
        useFec.store(settings.enableFec && !usb);
        Log(log, std::string("[Net] FEC ") + (useFec.load() ? "on" : "off") +
                     (usb ? " (USB tethering link)" : ""));
    };
    updateFec(settings.tabletIp);

    constexpr DWORD kPenSilenceTimeoutMs = 3000;
    auto resetPenSession = [&](const char* reason) {
        inputReceiver.ResetSourceLock();
        virtualPen.ReleaseIfDown();
        havePenSequence = false;
        {
            std::lock_guard<std::mutex> lock(penStateMutex);
            havePenPacket = false;
        }
        Log(log, std::string("[Pen] Session reset (") + reason + ") - ready for a new connection.");
    };

    // ---- Control channel: handshake, clock sync, PLI, client status ----
    control.SetCallback([&](const uint8_t* data, size_t len, const sockaddr_in& from) {
        ControlPacketView view;
        if (!ParseControlPacket(data, len, view)) return;
        const uint64_t now = SteadyMs();

        switch (view.type) {
        case kControlHello: {
            uint32_t assigned = 0;
            bool busy = false;
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                const bool sessionAlive = sessionActive && (now - sessionLastSeenMs) < kSessionTimeoutMs;
                if (sessionAlive && !SameHost(sessionAddress, from)) {
                    busy = true;  // One tablet at a time (spec appendix A.5).
                } else {
                    sessionActive = true;
                    sessionId = MakeSessionId();
                    sessionAddress = from;
                    sessionLastSeenMs = now;
                    assigned = sessionId;
                }
            }
            if (busy) {
                control.SendTo(from, BuildControlPacket(kControlBusy, 0));
                Log(log, "[Session] HELLO from " + IpToString(from) + " refused - already streaming to another tablet");
                break;
            }
            control.SendTo(from, BuildWelcome(assigned, kHostVersion));
            // The tablet that just said hello is by definition where the
            // video should go, which also covers the tethering-IP-changed
            // case without the user pressing anything.
            const std::string ip = IpToString(from);
            if (videoSender.SetDestination(ip)) {
                Log(log, "[Session] WELCOME id=" + std::to_string(assigned) + " -> " + ip +
                             "; video destination updated");
                updateFec(ip);
            }
            resetPenSession("new session");
            encoder.RequestKeyframe();
            break;
        }
        case kControlPing: {
            if (view.payloadLength < 8) break;
            const uint64_t t1 = ReadU64BE(view.payload);
            std::lock_guard<std::mutex> lock(sessionMutex);
            if (sessionActive && SameHost(sessionAddress, from)) {
                sessionLastSeenMs = now;
                sessionAddress = from;  // Track the current source port for replies.
            }
            control.SendTo(from, BuildPong(sessionId, t1, now, SteadyMs()));
            break;
        }
        case kControlPli: {
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                if (sessionActive && SameHost(sessionAddress, from)) {
                    sessionLastSeenMs = now;
                    sessionAddress = from;  // Current source port, for replies.
                }
            }
            pliTotal.fetch_add(1);
            // Coalesce: a burst of loss produces a burst of PLIs, and one
            // IDR already repairs all of them. Answering each one would
            // push a stream of large keyframes into a link that is already
            // dropping packets.
            const uint64_t last = lastKeyframeRequestMs.load();
            if (now - last >= kPliMinIntervalMs) {
                lastKeyframeRequestMs.store(now);
                encoder.RequestKeyframe();
            }
            break;
        }
        case kControlStatus: {
            ClientStatus status;
            if (!ParseStatus(view.payload, view.payloadLength, status)) break;
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                if (sessionActive && SameHost(sessionAddress, from)) {
                    sessionLastSeenMs = now;
                    sessionAddress = from;  // Current source port, for replies.
                }
            }
            {
                std::lock_guard<std::mutex> lock(statusMutex);
                latestStatus = status;
            }
            statusFresh.store(true);
            stats_.lossPermille.store(status.lossPermille, std::memory_order_relaxed);
            stats_.decodeQueue.store(status.decodeQueueLength, std::memory_order_relaxed);
            stats_.clientFps.store(status.completedFps, std::memory_order_relaxed);
            stats_.clientDecodeMs.store(status.decodeMsX10 / 10.0f, std::memory_order_relaxed);
            stats_.rttMs.store(status.rttMs, std::memory_order_relaxed);
            break;
        }
        case kControlTouch: {
            if (view.payloadLength < 1) break;
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                if (!sessionActive || !SameHost(sessionAddress, from)) break;
            }
            const int n = std::min<int>(view.payload[0], VirtualPen::kMaxTouchContacts);
            if (view.payloadLength < static_cast<size_t>(1 + 5 * n)) break;
            VirtualPen::TouchContact contacts[VirtualPen::kMaxTouchContacts];
            for (int i = 0; i < n; ++i) {
                const uint8_t* p = view.payload + 1 + 5 * i;
                contacts[i] = {p[0], ReadU16BE(p + 1) / 32767.0f, ReadU16BE(p + 3) / 32767.0f};
            }
            virtualPen.InjectTouch(contacts, n);
            break;
        }
        default:
            break;
        }
    });
    if (control.IsValid() && !control.Start()) {
        Log(log, "[WARN] Control channel thread failed to start");
    }

    // ---- Discovery responder (UDP 9999) ----
    UdpEndpoint discovery(settings.discoveryPort, true);
    if (discovery.IsValid()) {
        discovery.SetCallback([&](const uint8_t* data, size_t len, const sockaddr_in& from) {
            const std::string request(reinterpret_cast<const char*>(data), len);
            if (request.rfind(kDiscoveryRequest, 0) != 0) return;
            char hostname[256] = {};
            DWORD hostnameLen = sizeof(hostname);
            if (!GetComputerNameA(hostname, &hostnameLen)) std::strcpy(hostname, "PC");
            const std::string offer = std::string(kDiscoveryOfferPrefix) + hostname + "|" + kHostVersion;
            discovery.SendTo(from, reinterpret_cast<const uint8_t*>(offer.data()), offer.size());
            Log(log, "[Discovery] OFFER -> " + IpToString(from));
        });
        if (!discovery.Start()) Log(log, "[WARN] Discovery responder failed to start");
    } else {
        Log(log, "[WARN] Discovery port " + std::to_string(settings.discoveryPort) + " unavailable");
    }

    std::ostringstream ready;
    ready << "[PC] Streaming to " << settings.tabletIp << ":" << settings.videoPort
          << " (control " << settings.controlPort << ", FEC "
          << (useFec.load() ? "on" : "off") << ", QoS "
          << (settings.enableQos ? "on" : "off") << ")";
    Log(log, ready.str());

    uint16_t frameId = 0;
    uint32_t frameCounter = 0;

    QosController qos(settings.framerate, settings.bitrateKbps);
    stats_.qosFps.store(static_cast<uint32_t>(settings.framerate), std::memory_order_relaxed);
    stats_.qosBitrateKbps.store(static_cast<uint32_t>(settings.bitrateKbps), std::memory_order_relaxed);
    uint64_t nextQosMs = SteadyMs() + kQosIntervalMs;
    float lastEncodeMsAvg = 0.0f;

    // AcquireNextFrame()'s own timeout (see ScreenCapture::CaptureFrame) is
    // the pacer here - it blocks until DXGI actually has a new frame, which
    // tracks the display's real vsync/update cadence. Do NOT also gate this
    // loop with an external sleep_until(): a fixed external clock competing
    // with AcquireNextFrame's wait window causes it to miss frames that
    // arrive just after the external sleep already ate most of the budget,
    // silently dropping ~half the frames instead of a small amount of jitter.
    double captureMsTotal = 0, encodeMsTotal = 0, sendMsTotal = 0;
    double submitMsTotal = 0, readbackMsTotal = 0;
    double bitstreamBytesTotal = 0;
    uint32_t timedFrameCount = 0;
    uint32_t idleCount = 0;   // AcquireNextFrame timeouts: desktop unchanged
    uint32_t skipCount = 0;   // Frames dropped by the framerate cap
    uint32_t idrCount = 0;    // Keyframes emitted this second
    uint32_t keepaliveCount = 0;
    uint64_t lastVideoSentMs = SteadyMs();
    uint64_t lastIdrSentMs = SteadyMs();
    bool haveEncoderInput = false;

    auto sendEncodedFrame = [&](const std::vector<uint8_t>& encoded, uint64_t nowMs) {
        if (encoded.empty()) return false;
        uint32_t activeSession = 0;
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            activeSession = sessionActive ? sessionId : 0;
        }
        const auto timestamp = static_cast<uint32_t>(nowMs);
        auto packets = BuildVideoPackets(activeSession, frameId, timestamp, encoded);
        bool sent = !packets.empty();
        for (const auto& packet : packets) {
            if (!videoSender.Send(packet)) { sent = false; break; }
        }
        if (sent && useFec.load()) {
            for (const auto& parity : BuildFecPackets(activeSession, frameId, timestamp, encoded)) {
                if (!videoSender.Send(parity)) break;  // Parity is best-effort.
            }
        }
        ++frameId;
        if (sent) {
            ++frameCounter;
            lastVideoSentMs = nowMs;
        }
        if (encoder.LastWasIdr()) {
            ++idrCount;
            lastIdrSentMs = nowMs;
            stats_.idrKB.store(static_cast<uint32_t>(encoded.size() / 1024), std::memory_order_relaxed);
        }
        return sent;
    };

    // On a high-refresh monitor, DXGI can hand us frames faster than the
    // target framerate. Encoding/sending those extra frames burns GPU,
    // network, and queueing budget for no visual benefit - budget better
    // spent on latency. Cap by SKIPPING encode+send for frames that arrive
    // too soon after the last one we actually processed (not by sleeping
    // before AcquireNextFrame, which is what caused the missed-frame bug).
    auto frameInterval = std::chrono::microseconds(1000000 / std::max(1, qos.framerate()));
    // Skip only frames that are SUBSTANTIALLY early. A 60Hz desktop delivers
    // frames at almost exactly frameInterval, so comparing against the exact
    // interval makes microsecond-level jitter decide whether a frame is kept;
    // the ones rejected by a hair cost a full extra interval and drag the
    // rate down to ~40fps. The 15% margin keeps every frame at a matching
    // refresh rate while still dropping the genuine extras on a 120/144Hz
    // display.
    auto skipThreshold = frameInterval * 85 / 100;
    auto lastProcessedTime = std::chrono::steady_clock::now() - frameInterval;

    while (running_) {
        // Apply a pending manual retarget (from HostController::UpdateTabletIp,
        // called after "Detect tethering" finds the tablet at a new address
        // while already streaming). Cheap enough to check every iteration.
        {
            std::string newIp;
            bool hasIp = false;
            bool resetPen = false;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                if (hasPendingTabletIp_) { newIp = pendingTabletIp_; hasIp = true; hasPendingTabletIp_ = false; }
                resetPen = pendingPenReset_;
                pendingPenReset_ = false;
            }
            if (hasIp) {
                if (videoSender.SetDestination(newIp)) {
                    Log(log, "[PC] Video destination updated to " + newIp + ":" + std::to_string(settings.videoPort));
                    updateFec(newIp);
                } else {
                    Log(log, "[ERROR] Failed to update video destination to " + newIp);
                }
            }
            if (resetPen) resetPenSession("retargeted via Detect tethering");
        }

        const uint64_t loopNowMs = SteadyMs();

        // Session teardown: 3s without any control traffic means the tablet
        // is gone (app closed, link dropped). Drop the session so the next
        // HELLO is accepted from anywhere, and don't leave a pressed tip.
        {
            bool expired = false;
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                if (sessionActive && (loopNowMs - sessionLastSeenMs) >= kSessionTimeoutMs) {
                    sessionActive = false;
                    sessionId = 0;
                    expired = true;
                }
            }
            if (expired) {
                statusFresh.store(false);
                stats_.lossPermille.store(0, std::memory_order_relaxed);
                stats_.decodeQueue.store(0, std::memory_order_relaxed);
                stats_.clientFps.store(0, std::memory_order_relaxed);
                resetPenSession("control channel silent for 3s");
            }
        }
        bool haveSessionNow = false;
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            haveSessionNow = sessionActive;
            stats_.sessionActive.store(sessionActive, std::memory_order_relaxed);
            stats_.sessionId.store(sessionId, std::memory_order_relaxed);
        }
        if (!haveSessionNow && loopNowMs - lastIdrSentMs >= kNoSessionKeyframeIntervalMs) {
            encoder.RequestKeyframe();
        }

        // A locked pen source that has gone silent for kPenSilenceTimeoutMs
        // is what a mid-session reconnect from a new address looks like from
        // here - self-heal without waiting for the user to notice and press
        // Detect. (IsSourceLocked() is false right after resetPenSession()
        // above runs, so this doesn't immediately re-fire on the same event.)
        virtualPen.ReleaseStaleTouch(500);
        if (inputReceiver.IsSourceLocked() &&
            inputReceiver.MillisecondsSinceLastPacket() >= kPenSilenceTimeoutMs) {
            resetPenSession("no pen input for 3s");
        }

        const auto captureStart = std::chrono::steady_clock::now();
        const bool gotFrame = capture.CaptureFrame();
        const auto captureEnd = std::chrono::steady_clock::now();

        if (!gotFrame) ++idleCount;

        if (gotFrame && captureEnd - lastProcessedTime < skipThreshold) {
            ++skipCount;
            capture.ReleaseFrame();
            continue;
        }

        if (gotFrame) {
            lastProcessedTime = captureEnd;

            // Copy into the encoder's own texture and hand the frame straight
            // back to DXGI. Desktop Duplication cannot produce the next frame
            // while we still hold this one, so releasing it before the encode
            // and the send loop (rather than after) takes that dead time off
            // the end-to-end path.
            const bool copied = encoder.CopyInput(capture.GetFrameTexture());
            capture.ReleaseFrame();
            if (copied) haveEncoderInput = true;

            auto encoded = copied ? encoder.Encode() : std::vector<uint8_t>();
            const auto encodeEnd = std::chrono::steady_clock::now();
            sendEncodedFrame(encoded, SteadyMs());
            const auto sendEnd = std::chrono::steady_clock::now();

            captureMsTotal += std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();
            encodeMsTotal += std::chrono::duration<double, std::milli>(encodeEnd - captureEnd).count();
            sendMsTotal += std::chrono::duration<double, std::milli>(sendEnd - encodeEnd).count();
            submitMsTotal += encoder.LastSubmitMs();
            readbackMsTotal += encoder.LastReadbackMs();
            bitstreamBytesTotal += static_cast<double>(encoder.LastBitstreamBytes());
            ++timedFrameCount;
        } else if (haveEncoderInput && (SteadyMs() - lastVideoSentMs) >= kKeepaliveIntervalMs) {
            // Nothing on the desktop changed, so DXGI has no frame for us -
            // but a link that goes completely silent looks identical to a
            // disconnected one from the tablet's side. Re-encode the last
            // captured surface at 1fps as an ordinary P-frame: an unchanged
            // picture codes to almost nothing, whereas forcing an IDR here
            // threw away the quality the P-frames had built up and made a
            // static screen visibly pulse once a second. A tablet that joins
            // mid-quiet gets its keyframe from the HELLO handler instead.
            auto encoded = encoder.Encode();
            if (sendEncodedFrame(encoded, SteadyMs())) ++keepaliveCount;
        }

        // ---- QoS evaluation ----
        if (settings.enableQos && SteadyMs() >= nextQosMs) {
            nextQosMs = SteadyMs() + kQosIntervalMs;
            uint32_t loss = 0, queue = 0;
            if (statusFresh.load()) {
                std::lock_guard<std::mutex> lock(statusMutex);
                loss = latestStatus.lossPermille;
                queue = latestStatus.decodeQueueLength;
            }
            const auto decision = qos.Evaluate(lastEncodeMsAvg, loss, queue);
            if (decision.changed) {
                if (encoder.Reconfigure(decision.bitrateKbps, decision.framerate)) {
                    frameInterval = std::chrono::microseconds(1000000 / std::max(1, decision.framerate));
                    skipThreshold = frameInterval * 85 / 100;
                    qosDowngradeTotal.fetch_add(1);
                    stats_.qosFps.store(static_cast<uint32_t>(decision.framerate), std::memory_order_relaxed);
                    stats_.qosBitrateKbps.store(static_cast<uint32_t>(decision.bitrateKbps), std::memory_order_relaxed);
                    std::ostringstream qosLog;
                    qosLog << "[QoS] -> " << decision.framerate << "fps / " << decision.bitrateKbps
                           << "kbps (" << decision.reason << "; encode=" << lastEncodeMsAvg
                           << "ms loss=" << (loss / 10.0) << "% queue=" << queue << ")";
                    Log(log, qosLog.str());
                } else {
                    Log(log, "[QoS] Reconfigure rejected by the encoder - staying at current settings");
                }
            }
        }

        static auto statsAt = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - statsAt).count() >= 1) {
            std::ostringstream stats;
            stats << "[Stats] Frames: " << frameCounter << " fps";
            if (timedFrameCount > 0) {
                stats << " | avg capture=" << (captureMsTotal / timedFrameCount)
                      << "ms encode=" << (encodeMsTotal / timedFrameCount)
                      << "ms (submit=" << (submitMsTotal / timedFrameCount)
                      << " readback=" << (readbackMsTotal / timedFrameCount)
                      << ") send=" << (sendMsTotal / timedFrameCount) << "ms"
                      << " frame=" << static_cast<int>(bitstreamBytesTotal / timedFrameCount / 1024.0) << "KB";
            }
            // idle = seconds where the desktop simply did not change, so there
            // was nothing to capture. A low fps with a high idle count is not
            // a performance problem.
            stats << " | idle=" << idleCount << " capped=" << skipCount;
            if (keepaliveCount > 0) stats << " keepalive=" << keepaliveCount;
            // Recovery accounting: IDRs are the expensive frames, so their
            // count and size is what tells you whether PLI is thrashing.
            stats << " | idr=" << idrCount << " (" << stats_.idrKB.load() << "KB)"
                  << " pli=" << pliTotal.load();
            // The far end, as reported over the control channel. Without a
            // session these stay at zero.
            if (statusFresh.load()) {
                std::lock_guard<std::mutex> lock(statusMutex);
                stats << " | client " << latestStatus.completedFps << "fps loss="
                      << (latestStatus.lossPermille / 10.0) << "% queue="
                      << static_cast<int>(latestStatus.decodeQueueLength)
                      << " decode=" << (latestStatus.decodeMsX10 / 10.0) << "ms rtt="
                      << latestStatus.rttMs << "ms";
            }
            // Pressure actually delivered to Windows (0-1024). A varying
            // min..max here means the tablet -> host path is fine and any
            // remaining problem is in the receiving application.
            uint32_t penCount = 0, penMin = 0, penMax = 0, penLast = 0;
            const bool havePenData = virtualPen.ConsumePressureStats(penCount, penMin, penMax, penLast);
            if (havePenData) {
                stats << " | pen samples=" << penCount
                      << " pressure " << penMin << ".." << penMax
                      << " (last " << penLast << "/1024)";
            }
            Log(log, stats.str());

            // Same numbers, structured, for the GUI's stat tiles.
            stats_.fps.store(frameCounter, std::memory_order_relaxed);
            if (timedFrameCount > 0) {
                lastEncodeMsAvg = static_cast<float>(encodeMsTotal / timedFrameCount);
                stats_.captureMs.store(static_cast<float>(captureMsTotal / timedFrameCount), std::memory_order_relaxed);
                stats_.encodeMs.store(lastEncodeMsAvg, std::memory_order_relaxed);
                stats_.sendMs.store(static_cast<float>(sendMsTotal / timedFrameCount), std::memory_order_relaxed);
                stats_.frameKB.store(static_cast<uint32_t>(bitstreamBytesTotal / timedFrameCount / 1024.0), std::memory_order_relaxed);
            }
            stats_.idleCount.store(idleCount, std::memory_order_relaxed);
            stats_.cappedCount.store(skipCount, std::memory_order_relaxed);
            stats_.idrCount.store(idrCount, std::memory_order_relaxed);
            stats_.pliCount.store(pliTotal.load(), std::memory_order_relaxed);
            stats_.qosDowngrades.store(qosDowngradeTotal.load(), std::memory_order_relaxed);
            stats_.hasPenData.store(havePenData, std::memory_order_relaxed);
            if (havePenData) {
                stats_.penSamples.store(penCount, std::memory_order_relaxed);
                stats_.penMin.store(penMin, std::memory_order_relaxed);
                stats_.penMax.store(penMax, std::memory_order_relaxed);
                stats_.penLast.store(penLast, std::memory_order_relaxed);
            }
            frameCounter = 0;
            captureMsTotal = encodeMsTotal = sendMsTotal = 0;
            submitMsTotal = readbackMsTotal = bitstreamBytesTotal = 0;
            timedFrameCount = 0;
            idleCount = 0;
            skipCount = 0;
            idrCount = 0;
            keepaliveCount = 0;
            statsAt = std::chrono::steady_clock::now();
        }
    }

    // Pen first: its callback replies through the control endpoint, so
    // that thread has to be gone before the control socket closes.
    inputReceiver.Stop();
    discovery.Stop();
    control.Stop();
    virtualPen.ReleaseIfDown();
    virtualPen.ReleaseStaleTouch(0);
    Log(log, "[PC] Streaming stopped");
    running_ = false;
}

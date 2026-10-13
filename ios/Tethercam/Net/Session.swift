import AVFoundation
import Foundation
import UIKit

/// Device side of the Tethercam protocol (docs/PROTOCOL.md, DESIGN.md "Session state machine").
/// All state lives on `queue`; capture and encoding run on their own queues and hop back here.
final class Session: @unchecked Sendable {
    struct Stats: Sendable {
        var connected = false
        var streaming = false
        var width = 0
        var height = 0
        var fps = 0.0
        var mbps = 0.0
        var message = "Waiting for PC"
    }

    let camera = CameraController()
    private let queue = DispatchQueue(label: "tethercam.session")
    private lazy var server = Server(queue: queue)
    private let encoder = H264Encoder()
    private var parser = StreamParser()

    // Requested by the host (or picked on the phone); the lens is remembered across launches.
    private static let cameraKey = "selectedCamera"
    private var cameraId: UInt8 = UInt8(clamping: UserDefaults.standard.integer(forKey: Session.cameraKey)) {
        didSet { UserDefaults.standard.set(Int(cameraId), forKey: Session.cameraKey) }
    }
    private var mirror = false
    private var rotation: UInt16 = 0
    private var format = (width: UInt16(1920), height: UInt16(1080), fps: UInt16(30))
    private var requestedBitrate: UInt32 = 0
    private var streaming = false
    private var paused = false
    private var thermalLimited = false

    // Output state.
    private var configSent = false
    private var lastParameterSets = Data()
    private var dropUntilKeyframe = false
    private var heartbeatTimer: DispatchSourceTimer?
    private var windowStart = Date()
    private var windowBytes = 0
    private var windowFrames = 0
    private var stats = Stats()

    /// Called on the main queue whenever the stats change.
    var onStats: ((Stats) -> Void)?
    /// Called on the main queue when the host changes lens or the dim option.
    var onRemoteChange: ((UInt8, Bool) -> Void)?
    /// Called on the main queue when the host changes mirroring.
    var onRemoteMirror: ((Bool) -> Void)?

    private static let backpressureSeconds = 0.25

    // Read on the capture queue, written on `queue`: frames are only encoded while the PC streams.
    private let encodeLock = NSLock()
    private var encodeEnabled = false
    private var encoding: Bool {
        get { encodeLock.lock(); defer { encodeLock.unlock() }; return encodeEnabled }
        set { encodeLock.lock(); encodeEnabled = newValue; encodeLock.unlock() }
    }

    init() {
        camera.onFrame = { [weak self] buffer in
            guard let self, self.encoding else { return }
            self.encoder.encode(buffer)
        }
        encoder.onOutput = { [weak self] out in self?.queue.async { self?.send(frame: out) } }
        encoder.onError = { [weak self] status in
            self?.queue.async { self?.reportError(.encoderFailure, "encoder error \(status)") }
        }
    }

    func start() {
        queue.async { [self] in
            server.onConnect = { [weak self] in self?.connected() }
            server.onDisconnect = { [weak self] in self?.disconnected() }
            server.onData = { [weak self] data in self?.received(data) }
            server.onListenerState = { [weak self] text in self?.update { $0.message = text == "Listening" ? "Waiting for PC" : text } }
            server.start()
            startTimers()
            startPreview()
        }
        let center = NotificationCenter.default
        center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: nil) { [weak self] _ in
            self?.queue.async { self?.setPaused(true) }
        }
        center.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { [weak self] _ in
            self?.queue.async { self?.setPaused(false) }
        }
        center.addObserver(forName: .AVCaptureSessionWasInterrupted, object: camera.session, queue: nil) { [weak self] _ in
            self?.queue.async { self?.setPaused(true) }
        }
        center.addObserver(forName: .AVCaptureSessionInterruptionEnded, object: camera.session, queue: nil) { [weak self] _ in
            self?.queue.async { self?.setPaused(false) }
        }
        center.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: nil) { [weak self] _ in
            self?.queue.async { self?.thermalChanged() }
        }
        UIDevice.current.isBatteryMonitoringEnabled = true
    }

    /// Lens in use when the app starts (remembered from last time).
    var initialCamera: UInt8 { queue.sync { cameraId } }

    /// Mirror toggled on the phone.
    func setMirror(_ on: Bool) {
        queue.async { [self] in
            mirror = on
            camera.setOptions(mirror: mirror, rotation: rotation)
        }
    }

    /// Lens picked on the phone.
    func selectCamera(_ id: UInt8) {
        queue.async { [self] in
            cameraId = id
            if streaming { applyCapture() } else { startPreview() }
        }
    }

    /// Runs the camera for the on-screen preview while the PC isn't streaming (no encoding).
    private func startPreview() {
        guard !paused, !streaming else { return }
        encoding = false
        let formats = camera.cameras.first(where: { $0.id == cameraId })?.formats ?? camera.cameras.first?.formats ?? []
        guard let f = formats.first(where: { $0.width <= 1920 && $0.height <= 1080 }) ?? formats.last else { return }
        camera.start(cameraId: cameraId, width: f.width, height: f.height, fps: min(30, f.maxFps)) { _ in }
    }

    // MARK: Connection

    private func connected() {
        parser = StreamParser()
        configSent = false
        let device = UIDevice.current
        send(.hello(minVersion: Wire.version, maxVersion: Wire.version, role: .device,
                    appVersion: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0",
                    name: device.name, model: Self.modelIdentifier()))
        send(.caps(cameras: camera.cameras, codecs: 1))
        update { $0.connected = true; $0.message = "Connected" }
    }

    private func disconnected() {
        stopStreaming()
        update { $0.connected = false; $0.streaming = false; $0.message = "Waiting for PC" }
    }

    private func received(_ data: Data) {
        parser.feed(data)
        while true {
            switch parser.next() {
            case .needMore: return
            case .fatal:
                server.disconnect()
                return
            case .message(let m): handle(m)
            }
        }
    }

    private func handle(_ m: Message) {
        switch m {
        case let .hello(minV, maxV, _, _, _, _):
            if minV > Wire.version || maxV < Wire.version {
                send(.error(code: ErrorCode.unsupportedVersion.rawValue, message: "protocol version mismatch"))
                server.disconnect()
            }
        case let .setCamera(id):
            cameraId = id
            DispatchQueue.main.async { [weak self] in self?.onRemoteChange?(id, false) }
            if streaming { applyCapture() } else { startPreview() }
        case let .setFormat(w, h, fps, _, bitrate):
            format = (w, h, max(1, fps))
            requestedBitrate = bitrate
            if streaming { applyCapture() }
        case .start:
            streaming = true
            applyCapture()
        case .stop:
            stopStreaming()
        case .requestKeyframe:
            encoder.requestKeyframe()
            dropUntilKeyframe = true
        case let .setOptions(mirror, rotation, dim):
            self.mirror = mirror
            self.rotation = rotation
            camera.setOptions(mirror: mirror, rotation: rotation)
            DispatchQueue.main.async { [weak self] in self?.onRemoteMirror?(mirror) }
            configSent = false  // size may change with rotation; the encoder will emit a new config
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.onRemoteChange?(self.cameraId, dim)
            }
        default:
            break
        }
    }

    // MARK: Streaming

    private func effectiveFormat() -> (fps: Int, bitrateKbps: Int) {
        var fps = Int(format.fps)
        var bitrate = requestedBitrate > 0 ? Int(requestedBitrate) : Self.defaultBitrate(Int(format.width), Int(format.height), fps)
        if thermalLimited {
            fps = min(fps, 30)
            bitrate /= 2
        }
        return (fps, bitrate)
    }

    private func applyCapture() {
        guard !paused else { return }
        let f = effectiveFormat()
        encoder.configure(fps: f.fps, bitrateKbps: f.bitrateKbps)
        configSent = false
        camera.start(cameraId: cameraId, width: format.width, height: format.height, fps: UInt16(f.fps)) { [weak self] ok in
            guard let self else { return }
            self.queue.async {
                if ok {
                    self.encoding = self.streaming
                    self.update { $0.streaming = true; $0.width = Int(self.format.width); $0.height = Int(self.format.height); $0.message = "Streaming" }
                } else {
                    self.reportError(.invalidFormat, "cannot capture \(self.format.width)x\(self.format.height) @ \(self.format.fps)")
                }
            }
        }
    }

    private func stopStreaming() {
        streaming = false
        encoding = false
        encoder.invalidate()
        startPreview()
        configSent = false
        update { $0.streaming = false; $0.fps = 0; $0.mbps = 0; $0.message = $0.connected ? "Connected" : "Waiting for PC" }
    }

    private func setPaused(_ p: Bool) {
        guard paused != p else { return }
        paused = p
        if p {
            send(.error(code: ErrorCode.cameraUnavailable.rawValue, message: "Tethercam is in the background or the iPhone is locked"))
            update { $0.message = "Paused" }
        } else if streaming {
            camera.resume()
            applyCapture()
        } else {
            camera.resume()
            startPreview()
        }
    }

    private func thermalChanged() {
        let state = ProcessInfo.processInfo.thermalState
        let limited = state == .serious || state == .critical
        if state == .critical, streaming {
            send(.error(code: ErrorCode.thermalShutdown.rawValue, message: "iPhone is too hot; camera stopped"))
            stopStreaming()
            update { $0.message = "Too hot: let the iPhone cool down" }
            return
        }
        if limited != thermalLimited {
            thermalLimited = limited
            if streaming { applyCapture() }
        }
    }

    private func send(frame out: H264Encoder.Output) {
        guard streaming, !paused, server.isConnected else { return }
        // Backpressure: if more than ~250 ms of video is queued, drop until the next keyframe.
        let f = effectiveFormat()
        let budget = Int(Double(f.bitrateKbps) * 1000 / 8 * Self.backpressureSeconds)
        if server.bytesInFlight > max(budget, 512 * 1024) {
            if !dropUntilKeyframe { encoder.requestKeyframe() }
            dropUntilKeyframe = true
            return
        }
        if dropUntilKeyframe && !out.keyframe { return }
        dropUntilKeyframe = false

        if out.keyframe, let ps = out.parameterSets, !ps.isEmpty, !configSent || ps != lastParameterSets {
            send(.videoConfig(codec: 0, width: UInt16(encoder.width), height: UInt16(encoder.height),
                              fps: UInt16(f.fps), parameterSets: ps))
            lastParameterSets = ps
            configSent = true
        }
        guard configSent else { return }
        send(.videoFrame(ptsUs: out.ptsUs, keyframe: out.keyframe, data: out.annexB))
        windowBytes += out.annexB.count
        windowFrames += 1
    }

    private func send(_ m: Message) { server.send(m.encoded()) }

    private func reportError(_ code: ErrorCode, _ text: String) {
        send(.error(code: code.rawValue, message: text))
        update { $0.message = text }
    }

    // MARK: Timers and stats

    private func startTimers() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: 1)
        var tick = 0
        t.setEventHandler { [weak self] in
            guard let self else { return }
            tick += 1
            guard self.server.isConnected else { return }
            self.send(.heartbeat(sendTimeUs: UInt64(Date().timeIntervalSince1970 * 1_000_000)))
            let now = Date()
            let dt = now.timeIntervalSince(self.windowStart)
            if tick % 2 == 0, dt > 0 {
                let fps = Double(self.windowFrames) / dt
                let kbps = Double(self.windowBytes) * 8 / dt / 1000
                let battery = UIDevice.current.batteryLevel
                let charging = UIDevice.current.batteryState == .charging || UIDevice.current.batteryState == .full
                if self.streaming {
                    self.send(.status(thermal: Self.thermalByte(), battery: battery < 0 ? 255 : UInt8(battery * 100),
                                      charging: charging, bitrateKbps: UInt32(kbps), fpsX100: UInt16(min(65535, fps * 100)),
                                      cameraId: self.cameraId))
                }
                self.update { $0.fps = fps; $0.mbps = kbps / 1000 }
                self.windowStart = now
                self.windowBytes = 0
                self.windowFrames = 0
            }
        }
        t.resume()
        heartbeatTimer = t
    }

    private func update(_ change: (inout Stats) -> Void) {
        change(&stats)
        let snapshot = stats
        DispatchQueue.main.async { [weak self] in self?.onStats?(snapshot) }
    }

    static func defaultBitrate(_ w: Int, _ h: Int, _ fps: Int) -> Int {
        // Same table as windows/common/src/formats.cpp: 1080p30 = 12 Mbps, 4K30 = 40 Mbps, 60 fps ~1.7x.
        let base = 12000.0 * pow(Double(w * h) / (1920.0 * 1080.0), 0.87)
        return Int(min(150_000, max(2000, base * (fps > 30 ? 1.7 : 1.0))))
    }

    static func thermalByte() -> UInt8 {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: return 0
        case .fair: return 1
        case .serious: return 2
        case .critical: return 3
        @unknown default: return 0
        }
    }

    static func modelIdentifier() -> String {
        var info = utsname()
        uname(&info)
        return withUnsafeBytes(of: &info.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }
}

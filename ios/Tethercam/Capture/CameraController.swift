import AVFoundation
import Foundation
import UIKit

/// Owns the AVCaptureSession: lens discovery, capabilities, format selection, and frame delivery
/// (TECH_SPEC §5). All configuration happens on `sessionQueue`; frames arrive on `videoQueue`.
final class CameraController: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let session = AVCaptureSession()
    private let sessionQueue = DispatchQueue(label: "tethercam.camera")
    private let videoQueue = DispatchQueue(label: "tethercam.video", qos: .userInteractive)
    private let output = AVCaptureVideoDataOutput()
    private var input: AVCaptureDeviceInput?
    private var devices: [UInt8: AVCaptureDevice] = [:]
    private(set) var cameras: [CameraInfo] = []
    private var mirror = false
    /// Extra rotation requested by the PC (tray menu), added to the automatic angle.
    private var rotation: UInt16 = 0
    /// Angle that keeps the picture level for the way the phone is held (0 = landscape, home side right).
    private var autoAngle: CGFloat = 0
    private var rotationCoordinator: AnyObject?
    private var rotationObservation: NSKeyValueObservation?
    private var orientationObserver: NSObjectProtocol?

    /// Called on `videoQueue` for every captured frame.
    var onFrame: ((CMSampleBuffer) -> Void)?

    static let maxWidth: UInt16 = 3840
    static let maxHeight: UInt16 = 2160

    override init() {
        super.init()
        discover()
        if #unavailable(iOS 17.0) { observeDeviceOrientation() }
    }

    deinit {
        if let o = orientationObserver { NotificationCenter.default.removeObserver(o) }
    }

    // MARK: Orientation

    /// iOS 17+: follow the horizon-level angle for the active camera.
    private func trackRotation(of device: AVCaptureDevice) {
        guard #available(iOS 17.0, *) else { return }
        let coordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: nil)
        rotationCoordinator = coordinator
        rotationObservation = coordinator.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.initial, .new]) {
            [weak self] c, _ in
            let angle = c.videoRotationAngleForHorizonLevelCapture
            self?.sessionQueue.async {
                guard let self else { return }
                self.autoAngle = angle
                self.applyConnectionOptions()
            }
        }
    }

    /// iOS 16: map device orientation changes to a capture angle.
    private func observeDeviceOrientation() {
        DispatchQueue.main.async { [weak self] in
            UIDevice.current.beginGeneratingDeviceOrientationNotifications()
            self?.orientationObserver = NotificationCenter.default.addObserver(
                forName: UIDevice.orientationDidChangeNotification, object: nil, queue: .main
            ) { [weak self] _ in
                let angle: CGFloat
                switch UIDevice.current.orientation {
                case .landscapeLeft: angle = 0
                case .portrait: angle = 90
                case .landscapeRight: angle = 180
                case .portraitUpsideDown: angle = 270
                default: return  // face up/down or unknown: keep the last angle
                }
                self?.sessionQueue.async {
                    guard let self else { return }
                    self.autoAngle = angle
                    self.applyConnectionOptions()
                }
            }
        }
    }

    // MARK: Discovery

    private func discover() {
        let types: [AVCaptureDevice.DeviceType] = [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera]
        let found = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .unspecified).devices
        var id: UInt8 = 0
        // Back cameras first (wide, ultra wide, tele), then front.
        let order: [(AVCaptureDevice.Position, AVCaptureDevice.DeviceType, Lens, String)] = [
            (.back, .builtInWideAngleCamera, .wide, "Back Wide"),
            (.back, .builtInUltraWideCamera, .ultraWide, "Back Ultra Wide"),
            (.back, .builtInTelephotoCamera, .telephoto, "Back Telephoto"),
            (.front, .builtInWideAngleCamera, .front, "Front"),
        ]
        let zoom = Self.backZoomFactors()
        for (position, type, lens, label) in order {
            guard let device = found.first(where: { $0.position == position && $0.deviceType == type }) else { continue }
            let formats = Self.videoFormats(of: device)
            guard !formats.isEmpty else { continue }
            devices[id] = device
            // Back lenses carry their Camera-app zoom factor ("Back Telephoto 5×"); the PC shows it too.
            var name = label
            if position == .back, let factor = zoom[lens] { name += " " + Self.zoomText(factor) }
            cameras.append(CameraInfo(id: id, position: position == .front ? .front : .back, lens: lens,
                                      label: name, formats: formats))
            id += 1
        }
    }

    /// Zoom factors as the Camera app shows them (wide = 1×), from the multi-camera switch-over points.
    static func backZoomFactors() -> [Lens: Double] {
        var factors: [Lens: Double] = [.wide: 1]
        let session = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera], mediaType: .video, position: .back)
        guard let virtual = session.devices.first else { return factors }
        let points = virtual.virtualDeviceSwitchOverVideoZoomFactors.map { $0.doubleValue }
        switch virtual.deviceType {
        case .builtInTripleCamera where points.count >= 2:  // ultra wide, wide, tele
            factors[.ultraWide] = 1 / points[0]
            factors[.telephoto] = points[1] / points[0]
        case .builtInDualWideCamera where points.count >= 1:  // ultra wide, wide
            factors[.ultraWide] = 1 / points[0]
        case .builtInDualCamera where points.count >= 1:  // wide, tele
            factors[.telephoto] = points[0]
        default:
            break
        }
        return factors
    }

    static func zoomText(_ factor: Double) -> String {
        let rounded = (factor * 10).rounded() / 10
        let text = rounded == rounded.rounded() ? String(Int(rounded)) : String(format: "%.1f", rounded)
        return text + "\u{00D7}"
    }

    /// Unique 8-bit 4:2:0 video sizes with their highest frame rate (capped at 60), largest first.
    static func videoFormats(of device: AVCaptureDevice) -> [VideoFormat] {
        var best: [String: VideoFormat] = [:]
        for format in device.formats {
            let subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription)
            guard subtype == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                || subtype == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange else { continue }
            let dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            guard dims.width > 0, dims.height > 0, dims.width <= Int32(maxWidth), dims.height <= Int32(maxHeight) else { continue }
            let maxRate = format.videoSupportedFrameRateRanges.map(\.maxFrameRate).max() ?? 0
            let fps = UInt16(min(60, maxRate.rounded(.down)))
            guard fps >= 15 else { continue }
            let key = "\(dims.width)x\(dims.height)"
            let candidate = VideoFormat(width: UInt16(dims.width), height: UInt16(dims.height), maxFps: fps)
            if let existing = best[key], existing.maxFps >= fps { continue }
            best[key] = candidate
        }
        return best.values.sorted { Int($0.width) * Int($0.height) > Int($1.width) * Int($1.height) }
    }

    // MARK: Configuration

    /// Selects lens and format and starts capturing. Completion runs on the session queue.
    func start(cameraId: UInt8, width: UInt16, height: UInt16, fps: UInt16, completion: @escaping (Bool) -> Void) {
        sessionQueue.async { [self] in
            completion(configure(cameraId: cameraId, width: width, height: height, fps: fps))
        }
    }

    func stop() {
        sessionQueue.async { [self] in
            if session.isRunning { session.stopRunning() }
        }
    }

    /// Restarts capture after an interruption (e.g. the app returned to the foreground).
    func resume() {
        sessionQueue.async { [self] in
            if input != nil, !session.isRunning { session.startRunning() }
        }
    }

    func setOptions(mirror: Bool, rotation: UInt16) {
        sessionQueue.async { [self] in
            self.mirror = mirror
            self.rotation = rotation
            applyConnectionOptions()
        }
    }

    private func configure(cameraId: UInt8, width: UInt16, height: UInt16, fps: UInt16) -> Bool {
        guard let device = devices[cameraId] ?? devices.values.first else { return false }
        guard let format = Self.bestFormat(device: device, width: width, height: height, fps: fps) else { return false }

        session.beginConfiguration()
        defer { session.commitConfiguration() }
        session.sessionPreset = .inputPriority
        if input?.device != device {
            if let old = input { session.removeInput(old) }
            guard let newInput = try? AVCaptureDeviceInput(device: device), session.canAddInput(newInput) else { return false }
            session.addInput(newInput)
            input = newInput
            trackRotation(of: device)
        }
        if !session.outputs.contains(output) {
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            output.alwaysDiscardsLateVideoFrames = true
            output.setSampleBufferDelegate(self, queue: videoQueue)
            guard session.canAddOutput(output) else { return false }
            session.addOutput(output)
        }
        do {
            try device.lockForConfiguration()
            device.activeFormat = format
            let duration = CMTime(value: 1, timescale: CMTimeScale(fps))
            device.activeVideoMinFrameDuration = duration
            device.activeVideoMaxFrameDuration = duration
            if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
            if device.isExposureModeSupported(.continuousAutoExposure) { device.exposureMode = .continuousAutoExposure }
            device.unlockForConfiguration()
        } catch {
            return false
        }
        applyConnectionOptions()
        if !session.isRunning {
            // startRunning must not be called inside begin/commitConfiguration.
            sessionQueue.async { [self] in session.startRunning() }
        }
        return true
    }

    private func applyConnectionOptions() {
        guard let connection = output.connection(with: .video) else { return }
        if connection.isVideoStabilizationSupported { connection.preferredVideoStabilizationMode = .off }
        if connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = mirror
        }
        let total = (Int(autoAngle.rounded()) + Int(rotation)) % 360
        if #available(iOS 17.0, *) {
            let angle = CGFloat(total)
            if connection.isVideoRotationAngleSupported(angle) { connection.videoRotationAngle = angle }
        } else if connection.isVideoOrientationSupported {
            switch total {
            case 90: connection.videoOrientation = .portrait
            case 180: connection.videoOrientation = .landscapeLeft
            case 270: connection.videoOrientation = .portraitUpsideDown
            default: connection.videoOrientation = .landscapeRight
            }
        }
    }

    static func bestFormat(device: AVCaptureDevice, width: UInt16, height: UInt16, fps: UInt16) -> AVCaptureDevice.Format? {
        let matches = device.formats.filter { format in
            let dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            let subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription)
            return dims.width == Int32(width) && dims.height == Int32(height)
                && (subtype == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                    || subtype == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
                && format.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate }
        }
        // Prefer video range and formats without binning or HDR extras.
        return matches.sorted { a, b in
            let av = CMFormatDescriptionGetMediaSubType(a.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            let bv = CMFormatDescriptionGetMediaSubType(b.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            if av != bv { return av }
            return !a.isVideoBinned && b.isVideoBinned
        }.first
    }

    // MARK: AVCaptureVideoDataOutputSampleBufferDelegate

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        onFrame?(sampleBuffer)
    }
}

import AVFoundation
import SwiftUI

/// UI state for the single Tethercam screen (PRODUCT.md "iPhone app").
@MainActor
final class AppModel: ObservableObject {
    @Published var stats = Session.Stats()
    @Published var cameras: [CameraInfo] = []
    @Published var selectedCamera: UInt8 = 0
    @Published var mirrored = false
    @Published var dimmed = false
    @Published var permission: AVAuthorizationStatus = AVCaptureDevice.authorizationStatus(for: .video)
    /// Rotation for icons and labels so they stay upright while the UI is locked to portrait,
    /// like the Camera app.
    @Published var iconAngle: Angle = .zero

    let session = Session()
    private var started = false
    private var orientationObserver: NSObjectProtocol?

    init() {
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        orientationObserver = NotificationCenter.default.addObserver(
            forName: UIDevice.orientationDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.updateIconAngle() }
        }
    }

    private func updateIconAngle() {
        let angle: Double
        switch UIDevice.current.orientation {
        case .portrait: angle = 0
        case .landscapeLeft: angle = 90
        case .landscapeRight: angle = -90
        case .portraitUpsideDown: angle = 180
        default: return
        }
        withAnimation(.easeOut(duration: 0.25)) { iconAngle = .degrees(angle) }
    }

    /// Called when the screen appears and whenever the app becomes active (the user may have changed
    /// camera access in Settings meanwhile).
    func activate() async {
        UIApplication.shared.isIdleTimerDisabled = true
        permission = AVCaptureDevice.authorizationStatus(for: .video)
        if permission == .notDetermined {
            let granted = await AVCaptureDevice.requestAccess(for: .video)
            permission = granted ? .authorized : .denied
        }
        if permission == .authorized { startSession() }
    }

    private func startSession() {
        guard !started else { return }
        started = true
        cameras = session.camera.cameras
        let remembered = session.initialCamera
        selectedCamera = cameras.contains(where: { $0.id == remembered }) ? remembered : (cameras.first?.id ?? 0)
        session.onStats = { [weak self] stats in self?.stats = stats }
        session.onRemoteChange = { [weak self] camera, dim in
            withAnimation(.easeOut(duration: 0.2)) { self?.selectedCamera = camera }
            if dim { self?.dimmed = true }
        }
        session.onRemoteMirror = { [weak self] on in self?.mirrored = on }
        session.selectCamera(selectedCamera)
        session.start()
    }

    // MARK: Lenses

    var current: CameraInfo? { cameras.first { $0.id == selectedCamera } }
    var isFront: Bool { current?.position == .front }

    /// Lenses on the side in use, ordered like the Camera app (0.5×, 1×, tele).
    var lensChoices: [CameraInfo] {
        guard let side = current?.position, side == .back else { return [] }
        let order: [Lens: Int] = [.ultraWide: 0, .wide: 1, .telephoto: 2]
        return cameras.filter { $0.position == side }.sorted { (order[$0.lens] ?? 9) < (order[$1.lens] ?? 9) }
    }

    /// "0.5", "1×", "5" style text; the selected lens shows the × like the Camera app.
    func zoomLabel(_ c: CameraInfo, selected: Bool) -> String {
        let last = c.label.split(separator: " ").last.map(String.init) ?? ""
        var text = last.hasSuffix("\u{00D7}") ? String(last.dropLast()) : (c.lens == .ultraWide ? "0.5" : c.lens == .wide ? "1" : "2")
        if text.hasPrefix("0.") { text = String(text.dropFirst()) }  // ".5" like the Camera app
        return selected ? text + "\u{00D7}" : text
    }

    func select(_ id: UInt8) {
        guard id != selectedCamera else { return }
        withAnimation(.easeOut(duration: 0.2)) { selectedCamera = id }
        session.selectCamera(id)
    }

    private var lastBackCamera: UInt8?

    func flip() {
        if let c = current, c.position == .back { lastBackCamera = c.id }
        let target = isFront
            ? (cameras.first { $0.id == lastBackCamera } ?? cameras.first { $0.position == .back && $0.lens == .wide })
            : cameras.first { $0.position == .front }
        if let target { select(target.id) }
    }

    func toggleMirror() {
        mirrored.toggle()
        session.setMirror(mirrored)
    }

    var liveText: String {
        guard stats.streaming else { return stats.connected ? "Connected" : stats.message }
        let res = stats.height >= 2160 ? "4K" : "\(stats.height)p"
        return String(format: "%@ %.0f · %.1f Mbps", res, stats.fps, stats.mbps)
    }
}

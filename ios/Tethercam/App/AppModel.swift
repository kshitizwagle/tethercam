import AVFoundation
import SwiftUI

/// UI state for the single Tethercam screen (PRODUCT.md "iPhone app").
@MainActor
final class AppModel: ObservableObject {
    @Published var stats = Session.Stats()
    @Published var cameras: [CameraInfo] = []
    @Published var selectedCamera: UInt8 = 0
    @Published var dimmed = false
    @Published var permission: AVAuthorizationStatus = AVCaptureDevice.authorizationStatus(for: .video)

    let session = Session()
    private var started = false

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
        selectedCamera = cameras.first?.id ?? 0
        session.onStats = { [weak self] stats in self?.stats = stats }
        session.onRemoteChange = { [weak self] camera, dim in
            self?.selectedCamera = camera
            if dim { self?.dimmed = true }
        }
        session.start()
    }

    func select(_ id: UInt8) {
        selectedCamera = id
        session.selectCamera(id)
    }

    var statusText: String {
        guard stats.streaming else { return stats.message }
        return String(format: "Streaming %d×%d · %.0f fps · %.1f Mbps", stats.width, stats.height, stats.fps, stats.mbps)
    }
}

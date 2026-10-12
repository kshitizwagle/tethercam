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

    func onAppear() {
        UIApplication.shared.isIdleTimerDisabled = true
        switch permission {
        case .authorized:
            startSession()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { granted in
                Task { @MainActor in
                    self.permission = granted ? .authorized : .denied
                    if granted { self.startSession() }
                }
            }
        default:
            break
        }
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

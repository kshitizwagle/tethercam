import AVFoundation
import SwiftUI

struct ContentView: View {
    @StateObject private var model = AppModel()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if model.permission == .authorized {
                CameraPreview(session: model.session.camera.session)
                    .ignoresSafeArea()
                    .opacity(model.dimmed ? 0 : 1)
            } else if model.permission == .denied || model.permission == .restricted {
                VStack(spacing: 12) {
                    Image(systemName: "video.slash").font(.system(size: 44))
                    Text("Camera access is off").font(.title3.weight(.semibold))
                    Text("Allow camera access for Tethercam in Settings to use your iPhone as a webcam.")
                        .multilineTextAlignment(.center)
                        .foregroundStyle(.secondary)
                    Button("Open Settings") {
                        if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
                    }
                    .buttonStyle(.borderedProminent)
                }
                .padding(32)
                .foregroundStyle(.white)
            }

            VStack {
                HStack {
                    StatusPill(text: model.statusText, active: model.stats.streaming, connected: model.stats.connected)
                    Spacer()
                }
                Spacer()
                HStack(spacing: 12) {
                    if model.cameras.count > 1 {
                        Menu {
                            ForEach(model.cameras, id: \.id) { camera in
                                Button {
                                    model.select(camera.id)
                                } label: {
                                    if camera.id == model.selectedCamera {
                                        Label(camera.label, systemImage: "checkmark")
                                    } else {
                                        Text(camera.label)
                                    }
                                }
                            }
                        } label: {
                            ControlLabel(systemImage: "camera.rotate", text: "Lens")
                        }
                    }
                    Button {
                        model.dimmed.toggle()
                    } label: {
                        ControlLabel(systemImage: model.dimmed ? "eye" : "eye.slash",
                                     text: model.dimmed ? "Show preview" : "Hide preview")
                    }
                    Spacer()
                }
            }
            .padding(16)
        }
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .onAppear { model.onAppear() }
    }
}

private struct StatusPill: View {
    let text: String
    let active: Bool
    let connected: Bool

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(active ? Color.green : (connected ? Color.yellow : Color.gray))
                .frame(width: 8, height: 8)
            Text(text).font(.footnote.weight(.medium)).monospacedDigit()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.ultraThinMaterial, in: Capsule())
        .foregroundStyle(.white)
    }
}

private struct ControlLabel: View {
    let systemImage: String
    let text: String

    var body: some View {
        Label(text, systemImage: systemImage)
            .font(.footnote.weight(.medium))
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(.ultraThinMaterial, in: Capsule())
            .foregroundStyle(.white)
    }
}

/// Live camera preview backed by AVCaptureVideoPreviewLayer.
struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspect
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {}
}

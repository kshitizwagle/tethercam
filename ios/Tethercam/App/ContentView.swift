import AVFoundation
import SwiftUI

/// One screen, laid out like the Camera app: status on top, the preview, zoom pills over the
/// preview, the mode label, and a bottom bar with mirror · shutter · flip. The UI stays portrait;
/// icons rotate with the phone.
struct ContentView: View {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var scenePhase
    @State private var showWireless = false

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            switch model.permission {
            case .authorized: camera
            case .notDetermined: permissionPrompt
            default: permissionDenied
            }
        }
        .sheet(isPresented: $showWireless) { WirelessSheet(model: model) }
        .preferredColorScheme(.dark)
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .task { await model.activate() }
        .onChange(of: scenePhase) { phase in
            if phase == .active { Task { await model.activate() } }
        }
    }

    // MARK: Camera screen

    private var camera: some View {
        VStack(spacing: 0) {
            topBar
            qualityMenu
            ZStack(alignment: .bottom) {
                CameraPreview(session: model.session.camera.session)
                    .overlay(Color.black.opacity(model.dimmed ? 1 : 0))
                    .animation(.easeOut(duration: 0.25), value: model.dimmed)
                    .clipped()
                if !model.lensChoices.isEmpty && !model.dimmed {
                    zoomPills.padding(.bottom, 14)
                }
                if model.dimmed {
                    Text("Preview hidden to save battery · still streaming")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .frame(maxHeight: .infinity)
                }
            }
            bottomBar
        }
    }

    private var topBar: some View {
        ZStack {
            StatusPill(text: model.liveText, live: model.stats.streaming, connected: model.stats.connected)
                .rotationEffect(model.iconAngle)
            HStack {
                Spacer()
                Button { showWireless = true } label: {
                    Image(systemName: model.wireless ? "wifi" : "wifi.slash")
                        .font(.system(size: 17, weight: .medium))
                        .foregroundStyle(model.wireless ? Color.yellow : .white)
                        .frame(width: 44, height: 44)
                        .rotationEffect(model.iconAngle)
                }
                .accessibilityLabel("Wireless")
                .accessibilityValue(model.wireless ? "On" : "Off")
                .padding(.trailing, 8)
            }
        }
        .frame(height: 52)
    }

    private var qualityMenu: some View {
        Menu {
            Picker("Capture quality", selection: Binding(get: { model.quality }, set: { model.selectQuality($0) })) {
                ForEach(CaptureQuality.allCases, id: \.self) { quality in
                    Text(quality == .auto ? "Auto (match video app)" : quality.label)
                        .tag(quality)
                        .disabled(!quality.isSupported(by: model.current?.formats ?? []))
                }
            }
            Text("Capture quality; video apps choose their output size.")
            Text("4K uses more power and can warm your phone.")
        } label: {
            HStack(spacing: 6) {
                Text("Quality")
                Text(model.quality.label).foregroundStyle(.yellow)
                Image(systemName: "chevron.down").font(.caption)
            }
            .font(.subheadline.weight(.medium))
            .frame(minHeight: 44)
            .padding(.horizontal, 16)
        }
        .foregroundStyle(.white)
        .accessibilityLabel("Capture quality")
        .accessibilityValue(model.quality.label)
    }

    private var zoomPills: some View {
        HStack(spacing: 6) {
            ForEach(model.lensChoices, id: \.id) { lens in
                let selected = lens.id == model.selectedCamera
                Button { model.select(lens.id) } label: {
                    Text(model.zoomLabel(lens, selected: selected))
                        .font(.system(size: selected ? 13 : 11, weight: .semibold))
                        .foregroundStyle(selected ? Color.yellow : .white)
                        .frame(width: selected ? 38 : 30, height: selected ? 38 : 30)
                        .background(Circle().fill(.black.opacity(0.45)))
                        .rotationEffect(model.iconAngle)
                }
                .buttonStyle(PressScale())
            }
        }
        .padding(4)
        .background(Capsule().fill(.black.opacity(0.25)))
        .animation(.easeOut(duration: 0.2), value: model.selectedCamera)
    }

    private var bottomBar: some View {
        VStack(spacing: 18) {
            Text("WEBCAM")
                .font(.system(size: 13, weight: .semibold))
                .tracking(1.5)
                .foregroundStyle(.yellow)
                .padding(.top, 14)
            HStack {
                RoundIcon(systemName: "arrow.left.and.right.righttriangle.left.righttriangle.right",
                          active: model.mirrored, angle: model.iconAngle) { model.toggleMirror() }
                    .accessibilityLabel("Mirror")
                Spacer()
                Shutter(live: model.stats.streaming, dimmed: model.dimmed) { model.dimmed.toggle() }
                    .accessibilityLabel(model.dimmed ? "Show preview" : "Hide preview")
                Spacer()
                RoundIcon(systemName: "arrow.triangle.2.circlepath.camera", active: false, angle: model.iconAngle) {
                    model.flip()
                }
                .disabled(!model.cameras.contains { $0.position == .front })
                .accessibilityLabel("Switch camera")
            }
            .padding(.horizontal, 32)
            .padding(.bottom, 18)
        }
        .frame(maxWidth: .infinity)
        .background(Color.black)
    }

    // MARK: Permission states

    private var permissionPrompt: some View {
        VStack(spacing: 14) {
            Image(systemName: "video.fill").font(.system(size: 40))
            Text("Tethercam needs your camera").font(.title3.weight(.semibold))
            Text("Your iPhone becomes a webcam for your PC over the USB cable.")
                .multilineTextAlignment(.center).foregroundStyle(.secondary)
            Button("Allow camera") { Task { await model.activate() } }
                .buttonStyle(.borderedProminent).tint(.yellow).foregroundStyle(.black)
        }
        .padding(32)
    }

    private var permissionDenied: some View {
        VStack(spacing: 14) {
            Image(systemName: "video.slash.fill").font(.system(size: 40))
            Text("Camera access is off").font(.title3.weight(.semibold))
            Text("Allow camera access for Tethercam in Settings to use your iPhone as a webcam.")
                .multilineTextAlignment(.center).foregroundStyle(.secondary)
            Button("Open Settings") {
                if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
            }
            .buttonStyle(.borderedProminent).tint(.yellow).foregroundStyle(.black)
        }
        .padding(32)
    }
}

// MARK: Components

/// Wireless settings: the opt-in toggle and the PCs paired over USB.
private struct WirelessSheet: View {
    @ObservedObject var model: AppModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Wireless", isOn: Binding(get: { model.wireless }, set: { model.setWireless($0) }))
                } footer: {
                    Text("To pair a PC, turn this on and connect your iPhone to it with a USB cable once. After that, "
                         + "Tethercam connects to that PC over Wi-Fi whenever the cable is unplugged. Both need to be on "
                         + "the same network. Video is encrypted and only goes to PCs paired this way. A cable is faster "
                         + "and more reliable.")
                }
                if model.wireless && !model.stats.wifiState.isEmpty {
                    Section {
                        Text(model.stats.wifiState).foregroundStyle(.orange)
                        Button("Open Settings") {
                            if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
                        }
                    }
                }
                Section("Paired PCs") {
                    if model.pairedHosts.isEmpty {
                        Text("None yet").foregroundStyle(.secondary)
                    }
                    ForEach(model.pairedHosts) { host in
                        HStack {
                            Text(host.name.isEmpty ? "PC" : host.name)
                            Spacer()
                            Button("Forget", role: .destructive) { model.forget(host) }
                        }
                    }
                }
            }
            .navigationTitle("Wireless")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
    }
}

/// Buttons shrink slightly while pressed and spring back quickly.
private struct PressScale: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.92 : 1)
            .animation(.easeOut(duration: 0.15), value: configuration.isPressed)
    }
}

/// The recording-timer-style pill: red when live, grey otherwise.
private struct StatusPill: View {
    let text: String
    let live: Bool
    let connected: Bool

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(live ? Color.white : (connected ? Color.green : Color.gray)).frame(width: 6, height: 6)
            if live { Text("LIVE").font(.system(size: 13, weight: .bold)) }
            Text(text).font(.system(size: 13, weight: .medium)).monospacedDigit()
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(live ? Color.red : Color.white.opacity(0.12)))
        .foregroundStyle(.white)
        .animation(.easeOut(duration: 0.2), value: live)
    }
}

private struct RoundIcon: View {
    let systemName: String
    let active: Bool
    let angle: Angle
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 20, weight: .medium))
                .foregroundStyle(active ? Color.yellow : .white)
                .frame(width: 48, height: 48)
                .background(Circle().fill(Color.white.opacity(0.14)))
                .rotationEffect(angle)
        }
        .buttonStyle(PressScale())
        .animation(.easeOut(duration: 0.2), value: active)
    }
}

/// Shutter-style centre button: white ring; the inside is a red rounded square when live
/// (like a recording) and a white disc when idle. Tapping hides/shows the preview.
private struct Shutter: View {
    let live: Bool
    let dimmed: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack {
                Circle().stroke(Color.white, lineWidth: 4).frame(width: 74, height: 74)
                RoundedRectangle(cornerRadius: live ? 8 : 30)
                    .fill(live ? Color.red : Color.white)
                    .frame(width: live ? 30 : 60, height: live ? 30 : 60)
                    .opacity(dimmed ? 0.5 : 1)
            }
            .animation(.easeOut(duration: 0.25), value: live)
            .animation(.easeOut(duration: 0.2), value: dimmed)
        }
        .buttonStyle(PressScale())
    }
}

/// Live camera preview backed by AVCaptureVideoPreviewLayer (fills its area, like the Camera app).
struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.backgroundColor = .black
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspectFill
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {}
}

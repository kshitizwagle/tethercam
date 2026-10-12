import SwiftUI

// Skeleton only (M0). The camera preview, status pill and lens switcher come in M2.
struct ContentView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "cable.connector")
                .font(.system(size: 48))
            Text("Tethercam")
                .font(.title)
            Text("Waiting for PC")
                .foregroundStyle(.secondary)
        }
        .padding()
    }
}

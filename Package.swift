// swift-tools-version: 6.0
// The iPhone app as a SwiftPM package, for building on Linux with xtool (`xtool dev` from the repo root).
// Xcode and the macOS CI build the same sources from ios/project.yml; keep the two in step.
import PackageDescription

let package = Package(
    name: "Tethercam",
    platforms: [.iOS(.v16)],
    products: [.library(name: "Tethercam", targets: ["Tethercam"])],
    targets: [
        // Wi-Fi transport crypto; the Xcode build reaches it through the bridging header instead.
        .target(
            name: "CMonocypher",
            path: "third_party/monocypher",
            exclude: ["LICENCE.md"],
            publicHeadersPath: "."
        ),
        .target(
            name: "Tethercam",
            dependencies: ["CMonocypher"],
            path: "ios/Tethercam",
            exclude: ["Assets.xcassets", "Tethercam-Bridging-Header.h"],
            swiftSettings: [.swiftLanguageMode(.v5), .unsafeFlags(["-strict-concurrency=targeted"])]
        ),
    ]
)

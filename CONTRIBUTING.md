# Contributing to Tethercam

## Clean-room policy (read this first)

Tethercam is an independent, clean-room implementation, the way the ArtCraft apps are built. To keep the
project safe to distribute:

- **Do not** decompile, disassemble, debug, trace or packet-capture Camo or any other commercial webcam
  product, and don't contribute anything learned that way.
- **Do not** copy code, assets, UI text, icons or protocol details from those products, and don't try to
  make Tethercam compatible with their apps.
- **Do** use public sources:
  - Apple developer documentation (AVFoundation, VideoToolbox, Network.framework)
  - Microsoft documentation and samples (Media Foundation, `MFCreateVirtualCamera`, Frame Server)
  - Open-source projects with compatible licenses, used as references: libimobiledevice / usbmuxd
    (protocol behavior), smourier/VCamSample (MIT, Windows 11 virtual camera), OBS Studio
  - Your own experiments on your own hardware
- If a question can't be answered from public sources, add it to the **Open questions** list in
  [docs/TECH_SPEC.md](docs/TECH_SPEC.md) and design our own answer.

When you borrow from an open-source reference, say so in the commit message and keep its license notice
if any code is adapted.

## Code style

- **C++ (Windows):** C++20, MSVC, CMake. `clang-format` (repo `.clang-format`, based on LLVM style with
  4-space indent and a 110-column limit). Use RAII for every handle (`wil` or small local wrappers). No
  exceptions across COM boundaries; return `HRESULT`.
- **Swift (iOS):** Swift 6 language mode with strict concurrency. Keep capture and encoding off the main
  actor. `swift-format` defaults.
- Keep the wire protocol in [docs/PROTOCOL.md](docs/PROTOCOL.md) and the code in sync. A protocol change
  bumps the version and updates the doc in the same PR.

## Building

See [docs/DESIGN.md § Build](docs/DESIGN.md#build-and-ci). Short version:

```
# Windows
cd windows
cmake --preset vs2026-x64
cmake --build --preset debug
ctest --preset debug

# iOS (on a Mac)
cd ios && xcodegen && open Tethercam.xcodeproj
```

## Commits and PRs

- One logical change per PR. Mention the TODO.md item it closes.
- Tick the item in [TODO.md](TODO.md) in the same PR.

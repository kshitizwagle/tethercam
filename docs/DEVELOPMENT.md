# Developing Tethercam

Read [CONTRIBUTING.md](../CONTRIBUTING.md) first (clean-room policy). Architecture: [TECH_SPEC.md](TECH_SPEC.md)
and [DESIGN.md](DESIGN.md). Wire protocol: [PROTOCOL.md](PROTOCOL.md).

## Repository

| Path | What |
|---|---|
| `windows/common` | Platform-neutral C++: protocol codec, plist, usbmux client, sockets, shared-memory layout, image helpers, formats |
| `windows/media` | Media Foundation: H.264 decoder (hardware), encoder (for fakephone), placeholder renderer |
| `windows/vcam` | `tethercam-vcam.dll`, the Frame Server media source |
| `windows/agent` | `tethercam-agent.exe`: engine, tray, Direct2D window, status API, installer actions |
| `windows/tools` | `tcprobe`, `fakephone`, `vcamcheck` |
| `windows/tests` | GoogleTest unit tests |
| `windows/installer` | WiX 5 MSI |
| `ios/` | iPhone app (Swift, XcodeGen) |
| `altstore/` | AltStore source and icon (updated by CI) |
| `docs/protocol-vectors/v1.txt` | Golden protocol vectors shared by the C++ and Swift tests |
| `scripts/` | Icon generation, AltStore source updater |

## Windows build

Visual Studio 2026 (Desktop C++, Windows 11 SDK 10.0.26100+, includes CMake):

```
cd windows
cmake --preset vs2026-x64
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
```

MSI (WiX 5: `dotnet tool install --global wix --version 5.0.2`):

```
.\installer\build.ps1                 # -> build\vs2026-x64\Tethercam-<version>-x64.msi
```

The version is stamped at configure time: `-DTETHERCAM_BUILD_VERSION=1.2.3` (CI does this).

## iPhone build

On a Mac with Xcode 16+ and XcodeGen:

```
cd ios
xcodegen
open Tethercam.xcodeproj    # set your signing team, run on the iPhone
```

The project file is generated; never commit `Tethercam.xcodeproj`. On Windows, the Swift toolchain can
compile platform-neutral code (e.g. `ios/Tethercam/Protocol/Wire.swift`, `swiftc -swift-version 6`) to
check the protocol against the golden vectors; UIKit/AVFoundation code only builds in macOS CI.

## Running a development build

The camera's COM server must be registered machine-wide, and the DLL must be somewhere the FrameServer
service (LocalService) can read, not your user profile. From an **elevated** PowerShell:

```
windows\scripts\dev-install.ps1             # copies Debug binaries to C:\ProgramData\TethercamDev and registers
windows\scripts\dev-install.ps1 -Uninstall
```

Or test the media source without registering anything (`vcamcheck direct`, below).

## Without an iPhone

`fakephone` plays the iPhone's side of the protocol over plain TCP with an H.264 test pattern:

```
build\vs2026-x64\tools\fakephone\Debug\fakephone.exe
build\vs2026-x64\agent\Debug\tethercam-agent.exe --console --tcp 127.0.0.1:5321
```

## Tools

| Command | Use |
|---|---|
| `tcprobe list` | Devices Apple's USB service sees (`type=USB` vs `Network`) |
| `tcprobe stream [--tcp H:P] [--size WxH --fps N] [--decode --snapshot f.bmp] [--dump f.h264]` | Talk to the phone directly: caps, stats, decode check, raw dump. The agent must not be connected (one client at a time) |
| `tcprobe echo` | Raw throughput round trip through an echo server |
| `vcamcheck list` | Video devices as apps see them |
| `vcamcheck read [--size WxH --fps N --frames N --save f.bmp]` | Open the registered camera through Frame Server |
| `vcamcheck direct --dll tethercam-vcam.dll` | Load the media source in-process. Set `TETHERCAM_SHM_NAMESPACE=Local` for this and the agent |

Agent switches: `--console` (log to console, no tray), `--preview` (open the window at start),
`--tcp HOST:PORT`, `--software-decode`, `--install` / `--uninstall` (`--user`), `--session-camera`,
`--dll PATH`.

## Tests

- `ctest --preset debug`: protocol (golden vectors + fuzzing), plist, usbmux, formats, shared memory
  (concurrent seqlock), Annex-B, image conversion, version.
- iOS `WireTests` (macOS CI): the same golden vectors in Swift.
- `tcam_agent_tests`: real agent-loop quality negotiation, live changes and reconnect behavior over
  a loopback test peer; disables settings persistence and shared memory to isolate the user's camera.
- Measurement procedures and results: [perf.md](perf.md). App checks: [compat.md](compat.md).

## CI and releases

`.github/workflows/ci.yml` builds Windows (tests, Release, MSI) and iOS (simulator tests, unsigned
ad-hoc-signed IPA) on every push. When both pass on `main`, it tags the next `vX.Y.Z` (patch by default;
`[minor]` / `[major]` in the commit message), publishes the MSI, IPA and `SHA256SUMS.txt` as the
**latest** release, and adds the IPA to `altstore/source.json` (commit with `[skip ci]`). Pushes that
only touch docs (`**.md`, `docs/**`, `LICENSE`, `altstore/**`) don't build.

## Contracts other projects rely on

- **Status API** ([STATUS_API.md](STATUS_API.md)): port 4790, `/events` and the `live` field are used by
  the stream dashboard in `C:\Users\Admin\dev\streaming`. Add fields; don't rename or remove.
- **Wire protocol** ([PROTOCOL.md](PROTOCOL.md)): versioned; append fields only.

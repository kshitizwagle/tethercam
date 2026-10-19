# Developing Tethercam

Read [CONTRIBUTING.md](../CONTRIBUTING.md) first (clean-room policy). Architecture: [TECH_SPEC.md](TECH_SPEC.md)
and [DESIGN.md](DESIGN.md). Wire protocol: [PROTOCOL.md](PROTOCOL.md).

## Repository

| Path | What |
|---|---|
| `common` | Platform-neutral C++ shared by the Windows and Linux hosts: protocol codec, plist, usbmux client, sockets, image helpers, formats; shared-memory layout (Windows only) |
| `common/tests` | GoogleTest unit tests for `common` (run on Windows and Linux) |
| `windows/media` | Media Foundation: H.264 decoder (hardware), encoder (for fakephone), placeholder renderer |
| `windows/vcam` | `tethercam-vcam.dll`, the Frame Server media source |
| `windows/agent` | `tethercam-agent.exe`: engine, tray, Direct2D window, status API, installer actions |
| `windows/tools` | `tcprobe`, `fakephone`, `vcamcheck` |
| `windows/tests` | GoogleTest agent tests (Windows only) |
| `linux` | Linux host: `agent/` (engine, FFmpeg decode/encode, v4l2loopback output, tray, settings), `tests/`, `packaging/` (systemd user unit, modprobe config, maintainer scripts) |
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

## Linux build

GCC 13+ or Clang 17+, CMake 3.28+, FFmpeg and (optional, for the tray) GTK 3 + AyatanaAppIndicator:

```bash
sudo apt install cmake g++ pkg-config libavcodec-dev libavutil-dev libgtk-3-dev libayatana-appindicator3-dev
cd linux
cmake --preset linux && cmake --build --preset debug && ctest --preset debug
```

Fedora: `sudo dnf install cmake gcc-c++ ffmpeg-free-devel gtk3-devel libayatana-appindicator-gtk3-devel`.
Packages: configure a Release build, then `cpack -G DEB` or `cpack -G RPM` in the build directory
(`-DTETHERCAM_PACKAGE_TARGET=ubuntu24.04` names the file). Without a phone:

```bash
sudo modprobe v4l2loopback card_label=Tethercam exclusive_caps=1
build/linux/agent/fakephone --port 5321 &
build/linux/agent/tethercam-agent --tcp 127.0.0.1:5321
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=60     # any reader starts the stream
```

`fakephone` builds on Linux from `windows/tools/fakephone/main.cpp` with the FFmpeg encoder (libx264, or
OpenH264 on Fedora's ffmpeg-free).

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

On Linux, [xtool](https://github.com/xtool-org/xtool) builds, signs and installs the app from the repo root
(`Package.swift` + `xtool.yml`; needs `xtool setup` once for the Darwin SDK and Apple ID):

```
xtool dev run -c release --usb     # or: xtool dev build -c release --ipa
```

The package builds the same sources as `ios/project.yml` (no tests); `ios/xtool/Info.plist` repeats the
project's info properties, so keep the two in step.

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

The under-load harness wraps exactly that and prints the M10 numbers for an idle and a loaded run. The
camera repeats the last frame (or sends black) whenever the agent is late, so the sample rate always
reads ~60 fps; the harness therefore scores *new* frames (`vcamcheck` hashes a row of fakephone's moving
bars), gaps > 50 ms between them, the longest gap and the sample clock:

```
powershell -ExecutionPolicy Bypass -File windows\scripts\load-test.ps1           # load24, 20 s probes
powershell -ExecutionPolicy Bypass -File windows\scripts\load-test.ps1 -Load 0   # idle only
```

It tests whichever camera DLL is registered (the MSI or `dev-install.ps1`) through Frame Server with the
dev-tree agent, stops any running agent first, and shows the agent's `media thread:` and
`GPU scale ready:` lines (the latter only when the consumer size differs from the phone's). Record the
table in [perf.md](perf.md); the Windows-side checklist is [windows/scripts/LOAD-TEST.md](../windows/scripts/LOAD-TEST.md).

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
- Linux: `ctest --preset debug` in `linux/` runs the shared tests above (minus shared memory) plus
  `tcam_linux_tests` (settings, opener detection). The status API JSON contract is tested in
  `common/tests/agent_state_test.cpp` on both platforms.
- iOS `WireTests` (macOS CI): the same golden vectors in Swift.
- `tcam_agent_tests`: real agent-loop quality negotiation, live changes and reconnect behavior over
  a loopback test peer; disables settings persistence and shared memory to isolate the user's camera.
- Measurement procedures and results: [perf.md](perf.md). App checks: [compat.md](compat.md).

## CI and releases

`.github/workflows/ci.yml` builds Windows (tests, Release, MSI), Linux (`linux.yml`: tests and a `.deb`
or `.rpm` per target in Ubuntu 24.04, Ubuntu 26.04 and Fedora containers) and iOS (simulator tests, unsigned
ad-hoc-signed IPA) on every push. When all pass on `main`, it tags the next `vX.Y.Z` (patch by default;
`[minor]` / `[major]` in the commit message), publishes the MSI, Linux packages, IPA and `SHA256SUMS.txt` as the
**latest** release, and adds the IPA to `altstore/source.json` (commit with `[skip ci]`). Pushes that
only touch docs (`**.md`, `docs/**`, `LICENSE`, `altstore/**`) don't build.

## Contracts other projects rely on

- **Status API** ([STATUS_API.md](STATUS_API.md)): port 4790, `/events` and the `live` field are used by
  the stream dashboard in `C:\Users\Admin\dev\streaming`. Add fields; don't rename or remove.
- **Wire protocol** ([PROTOCOL.md](PROTOCOL.md)): versioned; append fields only.

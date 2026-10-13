# Tethercam: notes for coding sessions

Tethercam makes an iPhone a Windows 11 webcam over USB, at full video resolution. Video only, wired only,
no overlays. GPL-3.0.

## Start of every session
1. Read [MILESTONES.md](MILESTONES.md) for the current milestone, then **Next up** in [TODO.md](TODO.md).
2. Read only the doc sections the task needs: [docs/TECH_SPEC.md](docs/TECH_SPEC.md) (what/why),
   [docs/DESIGN.md](docs/DESIGN.md) (how), [docs/PROTOCOL.md](docs/PROTOCOL.md) (bytes on the wire).
3. Do one or two TODO items per session.

## End of every session
- **Commit when a milestone's work is done** (one commit per milestone, `M<n>: <summary>`). Don't commit
  half-finished milestones unless the user asks.
- Tick finished TODO items and update **Next up**.
- Add a dated line to the TODO.md session log.
- Record new findings in TECH_SPEC (risks / open questions) and keep DESIGN/PROTOCOL in sync with the code.

## Rules
- **Status API is a stable contract** (docs/STATUS_API.md): the stream dashboard in
  `C:\Users\Admin\dev\streaming` follows `http://127.0.0.1:4790/events` and the `live` field.
- **Portfolio page** (kshitizwagle.com.np/projects/tethercam) is built from `.portfolio/project.json` + images
  by the portfolio site (rebuilds every 5 h). Rules: images png/jpg/jpeg/webp/gif/avif (no SVG); paths
  relative to `.portfolio/`, segments start with a letter/digit, no `..`, `/` or `\`; every screenshot needs
  `alt`; links https, max 6; caps: stack 20, platforms 8, highlights 8 (title+text), screenshots 12;
  tagline ≤160, summary ≤1200, status ≤32 chars.
- **Don't edit Streamlabs scenes from this repo.** Scene changes belong to the streaming project
  (`C:\Users\Admin\dev\streaming`, see its CLAUDE.md: both canvases, locked items, hidden partners).
- **Clean room:** never decompile, trace or copy Camo or any other commercial webcam app. Use public
  Apple/Microsoft docs and permissively licensed references only (see CONTRIBUTING.md).
- Windows: C++20, CMake, MSVC, WIL for RAII, no exceptions across COM boundaries. `windows/common` stays
  free of COM/MF so it can be unit-tested.
- iOS: Swift 5 mode + strict-concurrency warnings, XcodeGen (`ios/project.yml`); never commit the generated `.xcodeproj`.
- The vcam DLL runs inside the FrameServer service as LocalService. It must not block, throw, write files,
  or trust shared-memory contents without validating them.
- The iOS app can't be built on the Windows PC. Leave iOS work clearly marked for the Mac.

## Testing without an iPhone
- `fakephone` simulates the device over TCP; run the agent with `--tcp 127.0.0.1:5321`.
- `vcamcheck direct --dll <tethercam-vcam.dll>` loads the camera in-process; set
  `TETHERCAM_SHM_NAMESPACE=Local` for both it and the agent.
- The Swift toolchain on the Windows PC can compile platform-neutral Swift (e.g. `ios/Tethercam/Protocol/Wire.swift`)
  with `swiftc -swift-version 6`; it needs `SDKROOT` set and `Swift\Runtimes\<ver>\usr\bin` on PATH to run.
  AVFoundation/UIKit code only builds in the macOS CI (`ios.yml`), which also produces the unsigned IPA.
- Registering the real camera needs admin: `windows/scripts/dev-install.ps1` (elevated) or the MSI.

## Build
```
cd windows
cmake --preset vs2026-x64
cmake --build --preset debug
ctest --preset debug
```
iOS (on a Mac): `cd ios && xcodegen && xcodebuild -scheme Tethercam -destination 'generic/platform=iOS' build`

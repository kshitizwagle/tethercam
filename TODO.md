# TODO

Work from the top. Tick items in the same commit that finishes them. Milestone exit criteria live in
[MILESTONES.md](MILESTONES.md).

## Next up
- [ ] M0 (on the Mac): `cd ios && xcodegen`, build and run the skeleton app on a phone; fix `project.yml` if needed
- [ ] M0: Create the GitHub repo, push, and confirm both CI workflows pass
- [ ] M1: Start the usbmuxd echo spike (Windows side can start now: Apple service is listening on 127.0.0.1:27015)

## M0: Foundations
- [x] PRODUCT.md, TECH_SPEC.md, DESIGN.md, PROTOCOL.md, PLAN.md, README, CONTRIBUTING, CLAUDE.md, LICENSE
- [x] Toolchain on the PC: Visual Studio 2026 (18.10) + C++ tools, Windows SDK 10.0.26100, CMake 4.3; Apple Mobile Device Service listening on 27015
- [x] `windows/CMakeLists.txt` + `CMakePresets.json` with `tcam_common`, `tethercam-agent`, `tethercam-vcam`, `tcprobe`, `fakephone`, `tcam_tests` (builds, test passes)
- [x] `.clang-format`, `.gitignore`
- [x] `ios/project.yml` (XcodeGen) with app + test targets, iOS 16, Swift 6; camera usage description (written, **not yet built on a Mac**)
- [x] `.github/workflows/windows.yml` (build + ctest)
- [x] `.github/workflows/ios.yml` (xcodegen + unsigned build + simulator tests) (not yet run: no GitHub remote)

## M1: Risk spikes
- [ ] `spikes/usbmux-echo/ios`: NWListener on 127.0.0.1:5321 echoing bytes
- [ ] `spikes/usbmux-echo/win`: usbmuxd client: ListDevices, Connect (byte-swapped port), echo 100 MB, report MB/s
- [ ] Record Q1 answer and throughput (Apple Devices vs iTunes if possible) in TECH_SPEC
- [ ] `spikes/vcam-pattern`: WRL media source DLL with moving NV12 pattern at 1080p30
- [ ] `spikes/vcam-pattern`: register/remove tool using `MFCreateVirtualCamera` (System lifetime)
- [ ] Confirm the camera appears and plays in the Windows Camera app; note Q2 findings
- [ ] Shared memory: DLL creates `Global\` mapping + events with the SDDL from TECH_SPEC §9
- [ ] User-mode tool writes frames into the mapping; Camera app shows them
- [ ] Update TECH_SPEC risks R1–R3 with results; pick fallback if needed

## M2: Stream
- [ ] Golden protocol vectors in `docs/protocol-vectors/` for every message type
- [ ] C++ protocol codec (`windows/common/protocol`) + tests
- [ ] C++ minimal plist XML writer/reader + tests
- [ ] C++ usbmux client (ListDevices, Listen, Connect) + framing tests
- [ ] Swift protocol codec + tests on the same vectors
- [ ] iOS `CameraController`: lens discovery, Caps generation, format selection
- [ ] iOS `H264Encoder`: VTCompressionSession low-latency settings, forced IDR
- [ ] iOS Annex-B packetizer + SPS/PPS extraction + tests
- [ ] iOS `Session` state machine, heartbeat, backpressure
- [ ] iOS UI: preview, status pill, lens switcher, dim toggle; idle timer off
- [ ] `tcprobe`: connect via usbmuxd or `--tcp`, print fps/bitrate, `--dump out.h264`
- [ ] `fakephone`: serve a `.h264` file as a device over TCP, honoring Start/Stop

## M3: Decode + preview
- [ ] Agent session thread (Hello, Caps, SetFormat, Start; reconnect loop)
- [ ] MF H.264 decoder MFT with D3D11 device manager, low-latency mode
- [ ] Decode-error handling → RequestKeyframe
- [ ] Debug preview window (`--preview`)
- [ ] Latency test procedure + first numbers in `docs/perf.md`

## M4: Webcam MVP
- [ ] `windows/vcam`: promote spike to Activator / MediaSource / MediaStream
- [ ] Shared-memory layout v1 (DESIGN.md) in `windows/common/shm` with seqlock + tests
- [ ] Agent shm writer; vcam shm reader with staleness check
- [ ] Placeholder frame rendering (Direct2D/DirectWrite → NV12)
- [ ] Demand-driven Start/Stop via control block
- [ ] `tethercam-agent --install / --uninstall` (COM registration + MFCreateVirtualCamera)
- [ ] Tray icon + menu (status, quit)
- [ ] Test in Camera app, Teams, Zoom; start `docs/compat.md`

## M5: Full resolution
- [ ] Advertise all F4 media types; format chooser (exact / nearest larger / largest)
- [ ] GPU scaling + letterbox via Video Processor MFT
- [ ] Bitrate table + user override (up to 150 Mbps)
- [ ] Lens switching, mirror, rotation (protocol `SetCamera` / `SetOptions`) from tray and phone
- [ ] Thermal policy on iOS
- [ ] 4K30/4K60 tests in OBS; latency + thermals + Lightning vs USB-C in `docs/perf.md`

## M6: Robustness
- [ ] usbmuxd `Listen` hot-plug handling
- [ ] iOS background/lock handling + "Paused on iPhone" placeholder
- [ ] Sleep/resume and agent crash recovery
- [ ] Multiple simultaneous consumers
- [ ] Parser fuzz tests in CI
- [ ] 1-hour soak test

## M7: Release v0.1
- [ ] WiX v4 MSI: Program Files install, regsvr32, vcam registration, agent autostart, uninstall cleanup
- [ ] User guide + troubleshooting (trust prompt, Apple Devices install, camera privacy settings)
- [ ] iOS build-from-source guide; decide on TestFlight (Q5)
- [ ] Release workflow with checksums; tag v0.1.0

## Session log
- 2026-10-12: Windows skeleton builds with VS 2026; iOS skeleton and CI written. M0 committed (iOS build + CI run still to verify).
- 2026-10-12: Project planned. Docs written (PRODUCT, TECH_SPEC, DESIGN, PROTOCOL, PLAN, MILESTONES, TODO).

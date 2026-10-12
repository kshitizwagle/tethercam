# Tethercam: Implementation Plan

The order of work is chosen to **retire the biggest risks first** (TECH_SPEC §11). The hard unknowns are
the Windows virtual camera plumbing and the USB tunnel, not the video encoding. Each phase ends with
something that runs and can be shown. Milestone exit criteria are in [MILESTONES.md](../MILESTONES.md),
and the task checklist is in [TODO.md](../TODO.md).

## Principles

- **Spikes before structure.** M1 spikes are throwaway-quality code in `spikes/`, deleted or promoted
  later. Their goal is answering a question, recorded in TECH_SPEC open questions.
- **No phone needed for most Windows work.** `fakephone` streams a recorded `.h264` over plain TCP, so the
  agent and vcam can be built and tested on any Windows 11 PC.
- **One vertical slice first** (1080p30, one lens), then widen to full resolution and robustness.
- **Docs move with code.** Protocol or shm layout changes update PROTOCOL.md / DESIGN.md in the same commit.

## Phases

### Phase 0: Foundations (M0)
Docs (this set), repo skeleton, CMake and XcodeGen projects that build empty targets, CI that builds both.

### Phase 1: Risk spikes (M1)
Three independent spikes that can be done in any order and in separate sessions:

1. **usbmuxd tunnel:** a minimal iOS app with an `NWListener` on 127.0.0.1:5321 echoing bytes, and a
   C++ console tool doing `ListDevices` + `Connect` + echo round trip. Answers Q1 and R3 (test with both
   Apple Devices and iTunes installs if possible).
2. **Virtual camera:** a DLL that serves a moving test pattern at 1080p30 NV12, registered with
   `MFCreateVirtualCamera`, visible in the Windows Camera app. Answers R2 and Q2.
3. **Session-0 shared memory:** the spike DLL creates `Global\` mapping + events with the DACL. A user-mode
   tool writes a colour into it and the Camera app shows that colour. Answers R1.

If spike 3 fails, choose the fallback (broker service or named pipe) and update TECH_SPEC before M2.

### Phase 2: Stream (M2)
Protocol codec in C++ and Swift with shared golden vectors. iOS capture + VideoToolbox H.264 at 1080p30,
packetizer, session state machine. `tcprobe` over usbmuxd saves a `.h264` that plays in ffplay. `fakephone`
can replay that file.

### Phase 3: Decode (M3)
Agent skeleton (no tray yet): session thread, MF hardware decoder, a debug preview window
(D3D11 swap chain). First latency measurement.

### Phase 4: Webcam MVP (M4)
Promote the vcam spike to `windows/vcam`. Agent writes decoded NV12 to shm; vcam serves it, with the
placeholder when idle. Demand-driven Start/Stop. Tray icon with status. Works in the Camera app, Teams and
Zoom at 1080p30. Start `docs/compat.md`.

### Phase 5: Full resolution (M5)
Caps-driven format list, format chooser, GPU scaler, 1440p/4K at 30/60, lens switching, mirror/rotation,
bitrate table and thermal policy. Benchmarks on a Lightning and a USB-C iPhone in `docs/perf.md`.

### Phase 6: Robustness (M6)
Hot-plug, lock/unlock, app background, PC sleep/resume, agent crash recovery, several consumers at once,
1-hour soak, fuzz tests for parsers.

### Phase 7: Release v0.1 (M7)
WiX MSI (installs to Program Files, registers the COM server + virtual camera, agent at login),
uninstall cleanup, user guide, troubleshooting, GitHub release with a checksum. iOS: TestFlight or
build-from-source instructions.

### Phase 8+: Backlog (M8)
Audio as a virtual microphone, HEVC option, Windows 10 DirectShow fallback, focus/exposure controls,
shared D3D11 textures instead of CPU copies, ARM64 build, choosing between several phones.

## Session workflow (many sessions over several weeks)

1. Read [CLAUDE.md](../CLAUDE.md), then the current milestone in MILESTONES.md and the top of TODO.md.
2. Pick the first unchecked item under **Next up**. Keep each session to one or two items.
3. Before ending: tick finished items, move the next items to **Next up**, add findings to TECH_SPEC open
   questions/risks, and leave a dated line in the TODO.md session log.

## Hardware and software needed

- Windows 11 PC: Visual Studio 2026 (includes CMake), the Apple Devices app (or iTunes), ffmpeg/ffplay (for checking dumps),
  OBS (for testing).
- Mac with Xcode 16+ and XcodeGen; a free Apple ID works for development (re-sign every 7 days).
- At least one iPhone (ideally both a Lightning and a USB-C model for M5 benchmarks) and a good data cable.

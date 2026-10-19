# Handoff: M11 Wi-Fi on Linux and iOS

M11 adds a Wi-Fi transport. You pair the phone once over USB, and after that it streams over Wi-Fi with the
cable unplugged. The design is in [docs/TECH_SPEC.md](docs/TECH_SPEC.md) §14, the wire format in
[docs/PROTOCOL.md](docs/PROTOCOL.md) ("Wireless"), and the checklist under M11 in
[MILESTONES.md](MILESTONES.md) and [TODO.md](TODO.md). The code is in the commit "M11 (wip)". It is written for
all three platforms. CI run 39188903816 (released as v0.1.15) built everything and passed all tests:
- Windows: 60/60
- Linux: 52/52 on Ubuntu 24.04/26.04 and Fedora, with Avahi found
- iOS: app build plus XCTest, including `NoiseTests`

What's left is the runtime and device checks below.

## What is verified (Windows PC, 2026-10-19)
- `ctest --preset debug`: 60/60 pass. Coverage:
  - Noise NNpsk0 against the cacophony vector
  - tamper and fuzz tests on the handshake, records, preamble and pairing file
  - `wifi_engine_test` (pairs over a plain link, then streams over Wi-Fi)
- End to end with fakephone:
  1. `fakephone --advertise` and `tethercam-agent --tcp` pair.
  2. A fresh agent finds the phone by Bonjour (DnsServiceBrowse) and connects on 5322.
  3. It streams 1080p30 through `vcamcheck direct` at 30.0 fps with no gaps over 50 ms.
  4. The Status API reports `"transport":"wifi"`.
- `ios/Tethercam/Net/Noise.swift` and `WifiChannel.swift` were compiled with swiftc + clang (Monocypher) on
  Windows. They pass the same vector, the record checks and the refusal checks.

## Linux: verified with the iPhone (2026-10-19)
Build + 52 tests with and without Avahi, USB pairing, Avahi discovery of the real phone, unplug → Wi-Fi
in 9 ms, 1080p60 at 20 Mbps in a Release build (docs/perf.md). Still open on Linux: replug → USB, Forget,
Wireless off, the tray Wireless submenu by eye. The notes below are the original checklist.

## Linux: original checklist
The code compiles and its tests pass in CI, but the agent has never been run on Linux. The Linux side shares `common/` (noise, link, pairing) with Windows,
so most risk is in the Linux-only files.

1. Build and test:
   ```
   cd linux && cmake --preset linux && cmake --build --preset debug && ctest --preset debug
   ```
   - New dependency: `libavahi-client-dev` (Debian/Ubuntu) or `avahi-devel` (Fedora). It's optional:
     `linux/agent/CMakeLists.txt` defines `TETHERCAM_AVAHI` only when pkg-config finds it, so also check that
     the build without it works.
   - `common/src/noise.cpp` uses `getrandom()` (`<sys/random.h>`).
   - `common/src/pairing.cpp` writes `$XDG_CONFIG_HOME/tethercam/pairing` with mode 0600.
2. Check these files first. They were written without a compiler:
   - `linux/agent/discovery.cpp`: Avahi browse/resolve on a threaded poll.
   - `linux/agent/engine.cpp`. It mirrors `windows/agent/engine.cpp`: `OpenLink`, `OpenWifi`, the Wi-Fi retry
     every 3 s, the usbmux check every 2 s during Wi-Fi sessions, and `WifiPair` after `Hello`.
   - `linux/agent/tray.cpp`: the GTK Wireless submenu and the `AskAddress` dialog.
3. Run end to end. fakephone is Windows-only, so run it on a Windows box on the same LAN or port its Wi-Fi bits:
   - `tethercam-agent --tcp <host>:5321 --pairing-file /tmp/p` pairs.
   - Then `tethercam-agent --pairing-file /tmp/p` finds the phone through Avahi, or use `--wifi <host>:5322`.
   - Read frames from the v4l2loopback device.
4. Packaging: the .deb recommends `avahi-daemon` (`linux/CMakeLists.txt`). The .rpm may want the same.

## iOS: still to do (on a Mac or in CI)
The app builds and its tests pass in CI (`ios.yml`). It has not been run on a device.

1. Build:
   ```
   cd ios && xcodegen && xcodebuild -scheme Tethercam -destination 'generic/platform=iOS' build
   ```
   Then run the tests (`NoiseTests`, plus `WireTests`, which now expects 20 vectors).
   - `project.yml` compiles `../third_party/monocypher` into the app target. Swift reaches it through
     `Tethercam/Tethercam-Bridging-Header.h` and `HEADER_SEARCH_PATHS`. If the bridging header fights with the
     test target, give `TethercamTests` the same `SWIFT_OBJC_BRIDGING_HEADER`.
   - Swift 5 mode with strict-concurrency warnings. Watch `Net/Server.swift` (rewritten: the Wi-Fi
     `NWListener` with the Bonjour service, a `WifiChannel` per connection, a 5 s handshake timeout) and
     `Net/Session.swift` (`setWireless`, `forgetHost`, `acceptPairing`, `pendingOffer`).
2. Check on a device:
   1. Turn Wireless on (Wi-Fi button in the top bar). iOS should show the Local Network prompt.
   2. Plug into the PC once. The agent sends `WifiPair`, and the phone lists the PC under "Paired PCs".
   3. Unplug. Within about 3 s the PC tray shows Wi-Fi, the status pill shows "· Wi-Fi", and video plays at
      1080p or lower in Auto.
   4. Plug back in mid-stream. It should switch to USB within about 2 s.
   5. Forget the PC on the phone. The Wi-Fi session should end and a reconnect should be refused.
   6. Turn Wireless off. Port 5322 and the Bonjour advert should go away.
   7. Measure latency and frame loss over Wi-Fi vs USB with the perf.md method, and record the results there.
3. Open questions: TECH_SPEC Q8 (does `DnsServiceBrowse` see the iOS advert on every network) and Q9 (when the
   Local Network prompt appears).

## When it all works
Tick the remaining M11 items in MILESTONES/TODO, add a session log line, delete this file and commit
`M11: wireless transport`.

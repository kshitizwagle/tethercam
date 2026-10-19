# Tethercam Wire Protocol, version 1

This is our own protocol. It runs over a single TCP stream between the Windows agent (the **host**) and the
iOS app (the **device**). The TCP stream is tunnelled over USB by Apple's usbmuxd service: the host asks
usbmuxd to connect to the device's port **5321**, and from then on the socket is a plain byte stream.
Over Wi-Fi (M11) the same byte stream runs inside an encrypted channel on port 5322; see
[Wireless transport](#wireless-transport-m11).
How usbmuxd itself works is described in [TECH_SPEC.md § USB transport](TECH_SPEC.md#4-usb-transport-usbmuxd).

## Conventions

- All integers are **little-endian**.
- `str` = `u16 byte_length` followed by UTF-8 bytes (no terminator).
- Unknown message types are ignored, so newer peers can add messages without breaking older ones.
  Unknown trailing bytes inside a known message are also ignored.
- A peer that receives a malformed header (bad magic, length > `MAX_PAYLOAD`) closes the connection.

## Frame header (12 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII `TCAM` (0x54 0x43 0x41 0x4D) |
| 4 | 1 | `version` | `1` |
| 5 | 1 | `type` | Message type, see below |
| 6 | 2 | `flags` | Reserved, send 0 |
| 8 | 4 | `length` | Payload length in bytes, `MAX_PAYLOAD` = 16 MiB |

The payload follows the header directly.

## Message types

| Type | Name | Direction | Purpose |
|---|---|---|---|
| 0x01 | `Hello` | both | Identify the peer and protocol range |
| 0x02 | `Caps` | device → host | Cameras and video formats the phone supports |
| 0x10 | `SetCamera` | host → device | Choose the lens |
| 0x11 | `SetFormat` | host → device | Choose resolution, frame rate, bitrate |
| 0x12 | `Start` | host → device | Start streaming |
| 0x13 | `Stop` | host → device | Stop streaming (camera may power down) |
| 0x14 | `RequestKeyframe` | host → device | Ask for an IDR frame as soon as possible |
| 0x15 | `SetOptions` | host → device | Mirror, rotation, preview dimming |
| 0x16 | `SetQuality` | both | Synchronize the saved capture-quality preference |
| 0x20 | `VideoConfig` | device → host | Codec parameters (SPS/PPS) for the frames that follow |
| 0x21 | `VideoFrame` | device → host | One encoded access unit |
| 0x30 | `Heartbeat` | both | Liveness, every 1 s |
| 0x31 | `Status` | device → host | Thermal state, battery, actual bitrate |
| 0x40 | `WifiPair` | host → device, USB only | Hand over the Wi-Fi pairing key (M11) |
| 0x41 | `WifiPaired` | device → host, USB only | Pairing result and the phone's Wi-Fi address (M11) |
| 0x7F | `Error` | both | Report a problem |

### 0x16 SetQuality

One `u8 quality`: 0 = Auto, 1 = 1080p30, 2 = 1080p60, 3 = 4K30, 4 = 4K60.
A missing value is malformed. A value this build doesn't know (a preset added by a newer peer) makes
the receiver ignore the message, so new presets never break older peers. Unknown trailing bytes are ignored.

The device sends its saved quality after Hello and before Caps, and again when the phone UI changes
it. The host adopts it unless a local desktop change is pending. A host-side change sends SetQuality
to the device (which persists it without echoing). When streaming is wanted, the host resolves the
preference against the selected lens's Caps, then sends Stop, SetCamera, SetOptions, SetFormat, Start.
SetQuality alone does not start capture. Auto uses consumer demand; explicit presets override source
capture only. The virtual camera still delivers its negotiated output size/rate through scaling/pacing.

The phone's saved choice wins on reconnect unless the PC has an unsent local choice. Older v1 peers
ignore this message: an updated PC can still request a preset using SetFormat, but phone-side quality
selection requires an updated Windows companion. Both apps should be updated for synchronized controls.

### 0x01 Hello

| Field | Type | Notes |
|---|---|---|
| `min_version` | u8 | Lowest protocol version this peer accepts |
| `max_version` | u8 | Highest |
| `role` | u8 | 0 = host, 1 = device |
| `app_version` | str | e.g. `0.1.0` |
| `name` | str | Device name (`Alex's iPhone`) or host name |
| `model` | str | e.g. `iPhone16,1`, or `Windows 11 26100` |

Both sides send `Hello` right after connecting. The session uses the highest version in both ranges. If
there's none, the side that notices sends `Error(UNSUPPORTED_VERSION)` and closes.

### 0x02 Caps

| Field | Type | Notes |
|---|---|---|
| `camera_count` | u8 | |
| `cameras[]` | | repeated `camera_count` times |
| ↳ `camera_id` | u8 | Stable for this connection |
| ↳ `position` | u8 | 0 = back, 1 = front |
| ↳ `lens` | u8 | 0 = wide, 1 = ultra-wide, 2 = telephoto, 3 = front (TrueDepth) |
| ↳ `label` | str | Human-readable, e.g. `Back Ultra Wide` |
| ↳ `format_count` | u16 | |
| ↳ `formats[]` | | repeated `format_count` times |
| ↳↳ `width` | u16 | Landscape orientation, e.g. 3840 |
| ↳↳ `height` | u16 | e.g. 2160 |
| ↳↳ `max_fps` | u16 | Highest supported frame rate for this size, e.g. 60 |
| `codecs` | u8 | Bitmask: bit0 = H.264, bit1 = HEVC |

The device collapses `AVCaptureDevice.formats` to unique (width, height) pairs with the highest frame
rate for each, using only formats suitable for video (not photo-only or high-frame-rate slow-motion).

### 0x10 SetCamera

| Field | Type |
|---|---|
| `camera_id` | u8 |

### 0x11 SetFormat

| Field | Type | Notes |
|---|---|---|
| `width` | u16 | Must match a format in `Caps` |
| `height` | u16 | |
| `fps` | u16 | ≤ `max_fps` |
| `codec` | u8 | 0 = H.264, 1 = HEVC |
| `bitrate_kbps` | u32 | 0 = device chooses (see TECH_SPEC bitrate table) |

### 0x12 Start / 0x13 Stop / 0x14 RequestKeyframe

Empty payloads.

### 0x15 SetOptions

| Field | Type | Notes |
|---|---|---|
| `mirror` | u8 | 0/1, flip horizontally before encoding |
| `rotation` | u16 | 0, 90, 180, 270 degrees, applied on the device |
| `dim_preview` | u8 | 0/1, dim or blank the on-phone preview |

### 0x20 VideoConfig

Sent before the first `VideoFrame` and again whenever the codec parameters change (new format, lens or
rotation). The next frame after a `VideoConfig` is always a keyframe.

| Field | Type | Notes |
|---|---|---|
| `codec` | u8 | 0 = H.264, 1 = HEVC |
| `width` | u16 | Encoded size after rotation |
| `height` | u16 | |
| `fps` | u16 | Nominal frame rate |
| `parameter_sets` | u32 length + bytes | Annex-B (`00 00 00 01` start codes): SPS + PPS (+ VPS for HEVC) |

### 0x21 VideoFrame

| Field | Type | Notes |
|---|---|---|
| `pts_us` | u64 | Capture timestamp in microseconds, device clock (monotonic) |
| `frame_flags` | u8 | bit0 = keyframe (IDR) |
| `data` | rest of payload | One access unit, Annex-B NAL units |

### 0x30 Heartbeat

| Field | Type | Notes |
|---|---|---|
| `send_time_us` | u64 | Sender's monotonic clock; the peer echoes nothing, it's for logging and drift estimates |

A peer that hears nothing (no message of any kind) for **3 s** treats the connection as dead.

### 0x31 Status

| Field | Type | Notes |
|---|---|---|
| `thermal_state` | u8 | 0 nominal, 1 fair, 2 serious, 3 critical (`ProcessInfo.ThermalState`) |
| `battery_percent` | u8 | 0–100, 255 = unknown |
| `charging` | u8 | 0/1 |
| `actual_bitrate_kbps` | u32 | Measured over the last second |
| `actual_fps` | u16 | ×100, e.g. 2997 |
| `camera_id` | u8 | Lens currently in use (`Caps` id), 255 = unknown. Appended in app 0.1.4; older devices omit it |

Sent every 2 s while streaming. The host remembers `camera_id` so a lens picked on the phone survives reconnects.

### 0x7F Error

| Field | Type |
|---|---|
| `code` | u16 |
| `message` | str |

| Code | Meaning |
|---|---|
| 1 | `UNSUPPORTED_VERSION` |
| 2 | `CAMERA_PERMISSION_DENIED` |
| 3 | `INVALID_FORMAT` |
| 4 | `CAMERA_UNAVAILABLE` (in use or app backgrounded) |
| 5 | `ENCODER_FAILURE` |
| 6 | `THERMAL_SHUTDOWN` |

## Typical session

```
host                                device
 |---- usbmuxd Connect(port 5321) --->|
 |---- Hello ------------------------>|
 |<--------------------------- Hello -|
 |<---------------------------- Caps -|
 |---- SetCamera(0) ----------------->|
 |---- SetFormat(1920,1080,30,H264,0)>|
 |---- Start ------------------------>|
 |<--------------------- VideoConfig -|
 |<------------- VideoFrame (IDR) ----|
 |<------------- VideoFrame ----------|   ... ~30/s
 |<---- Heartbeat / Status -----------|
 |---- SetFormat(3840,2160,30,...) -->|   app re-opened the camera at 4K
 |<--------------------- VideoConfig -|
 |<------------- VideoFrame (IDR) ----|
 |---- Stop ------------------------->|   last app closed the camera
```

## Wireless transport (M11)

Pairing happens only over USB (`WifiPair` / `WifiPaired` below). Over Wi-Fi the host then connects to the
phone's port **5322** directly (the address the phone reported in `WifiPaired`, its Bonjour
`_tethercam._tcp` advert, or an address the user entered). The connection starts with a preamble and a
Noise handshake on the pairing key; everything after the handshake is encrypted. Implementations:
`common/src/link.cpp` + `noise.cpp` (hosts, fakephone) and `ios/Tethercam/Net/WifiChannel.swift` +
`Noise.swift` (phone), both on Monocypher.

### 0x40 WifiPair (host → device, USB only)

Sent after `Hello` in every USB session.

| Field | Type | Notes |
|---|---|---|
| `host_id` | 16 bytes | Random, generated once per PC user (kept in the host's pairing file) |
| `key` | 32 bytes | Random, generated once per PC–phone pair, kept by the host under the phone's USB serial |
| `name` | str | Host display name (computer name) |

A device ignores `WifiPair` arriving over Wi-Fi. Older devices ignore the type.

### 0x41 WifiPaired (device → host, USB only)

The reply; sent again if Wireless is turned on later in the same USB session.

| Field | Type | Notes |
|---|---|---|
| `status` | u8 | 0 = paired (key stored), 1 = Wireless is off (nothing stored) |
| `device_id` | 16 bytes | Random, generated once per phone install; also the Bonjour TXT `id` (hex) |
| `name` | str | Phone display name |
| `port` | u16 | Wi-Fi port, 5322 |
| `address` | str | Current Wi-Fi IPv4 address, empty if unknown |

### Bonjour

The phone advertises `_tethercam._tcp` on the Wi-Fi port while Wireless is on, TXT `v=1` and
`id=<device_id as lowercase hex>`. Hosts browse only once they have a pairing.

### Preamble (host → device, 24 bytes, plaintext)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII `TCWF` |
| 4 | 1 | `version` | `1` |
| 5 | 3 | reserved | send 0, ignore |
| 8 | 16 | `host_id` | Selects the pairing key |

Bad magic, an unknown version or an unknown `host_id` → the device closes the connection.

### Session handshake (Noise NNpsk0)

`Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b` (noiseprotocol.org rev. 34), host = initiator, PSK = the pairing
key, prologue = the 24 preamble bytes, empty payloads. Each handshake message is sent as `u16 LE length`
+ bytes (48 bytes each). A handshake that fails authentication closes the connection; the phone also
drops a connection that hasn't finished the handshake within 5 s.

### Records

After the handshake both directions carry `u32 LE length` + ciphertext (Noise transport messages:
ChaCha20-Poly1305, nonce = 32 zero bits + 64-bit LE counter, empty AD; plaintext ≤ 65 519 bytes + 16-byte
tag). The plaintexts concatenate into the ordinary TCAM v1 byte stream described above; message
boundaries don't have to line up with records. A record that fails authentication, or a length outside
16..65 535, closes the connection.

### Test vectors

Both implementations pass the public `Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b` vector from the cacophony
suite (`common/tests/wifi_test.cpp`, `ios/TethercamTests/NoiseTests.swift`); `WifiPair`/`WifiPaired` are in
`docs/protocol-vectors/v1.txt`.

## Versioning

- Adding a message type or appending fields to the end of a message does **not** change the version.
- Changing the meaning or layout of existing fields **does**: bump `version` and keep handling the old one
  for at least one release.

# Tethercam Wire Protocol, version 1

This is our own protocol. It runs over a single TCP stream between the Windows agent (the **host**) and the
iOS app (the **device**). The TCP stream is tunnelled over USB by Apple's usbmuxd service: the host asks
usbmuxd to connect to the device's port **5321**, and from then on the socket is a plain byte stream.
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
| 0x20 | `VideoConfig` | device → host | Codec parameters (SPS/PPS) for the frames that follow |
| 0x21 | `VideoFrame` | device → host | One encoded access unit |
| 0x30 | `Heartbeat` | both | Liveness, every 1 s |
| 0x31 | `Status` | device → host | Thermal state, battery, actual bitrate |
| 0x7F | `Error` | both | Report a problem |

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

Sent every 2 s while streaming.

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

## Versioning

- Adding a message type or appending fields to the end of a message does **not** change the version.
- Changing the meaning or layout of existing fields **does**: bump `version` and keep handling the old one
  for at least one release.

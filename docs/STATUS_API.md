# Tethercam status API

> **Stable contract.** The stream dashboard in `C:\Users\Admin\dev\streaming` depends on this API:
> port **4790**, `GET /events` (SSE) and the **`live`** field. Don't change or remove them. Add new
> fields freely (consumers ignore unknown ones).

The Windows agent serves the camera's state on the loopback interface so stream overlays (OBS /
Streamlabs browser sources) can react, for example by showing their own "camera offline" design.
While the camera isn't live, the Tethercam virtual camera itself only outputs **black** (the text
status card is opt-in from the tray menu), so the overlay decides what viewers see.

- Address: `http://127.0.0.1:4790` (loopback only, read-only, `Access-Control-Allow-Origin: *`)
- `GET /status`: the current status as JSON
- `GET /events`: Server-Sent Events. The status JSON is sent on connect and again on every change
  (about once a second while live, because fps and bitrate change). A `: keep-alive` comment comes
  every 15 s. `EventSource` reconnects on its own if the agent restarts.

## Status object

```json
{
  "state": "live",
  "live": true,
  "device": "iPhone",
  "camera": "Front",
  "lens": "front",
  "width": 1920,
  "height": 1080,
  "fps": 30,
  "mirror": true,
  "rotation": 0,
  "detail": "1920x1080 @ 30 fps, 11.8 Mbps",
  "thermal": 0,
  "battery": 87,
  "charging": true,
  "version": "0.1.7"
}
```

| Field | Meaning |
|---|---|
| `state` | `live` (video flowing), `connected` (phone connected, no app using the camera), `no_iphone` (no iPhone on USB), `app_not_open` (iPhone found, Tethercam app closed/locked), `paused` (app in background or phone locked), `no_apple_driver` (Apple Devices not installed), `starting` |
| `live` | `true` only while frames are flowing. **Use this to show/hide your camera frame** |
| `device` | iPhone name |
| `camera`, `lens` | Lens in use: `wide`, `ultra_wide`, `telephoto`, `front` |
| `width`, `height`, `fps` | Format being streamed (0 when not live) |
| `mirror`, `rotation` | Current picture options |
| `detail` | Human-readable summary |
| `thermal`, `battery`, `charging` | iPhone thermal state (0 nominal, 1 fair, 2 serious, 3 critical; -1 unknown), battery percent (-1 unknown), charging. Updated every 2 s while live |

## Overlay example

```html
<div id="cam-frame" style="display:none">…your camera frame…</div>
<div id="cam-offline">…your "be right back" / camera-off design…</div>
<script>
  const frame = document.getElementById('cam-frame');
  const offline = document.getElementById('cam-offline');
  const show = (s) => {
    frame.style.display = s.live ? '' : 'none';
    offline.style.display = s.live ? 'none' : '';
  };
  const es = new EventSource('http://127.0.0.1:4790/events');
  es.onmessage = (e) => show(JSON.parse(e.data));
  es.onerror = () => show({ live: false });  // agent not running
</script>
```

Polling also works: `fetch('http://127.0.0.1:4790/status').then(r => r.json())`.

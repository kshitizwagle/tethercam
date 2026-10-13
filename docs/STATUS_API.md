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
| `camera` | Human-readable camera label; empty before a camera is selected |
| `lens` | `wide`, `ultra_wide`, `telephoto`, `front`; empty before a camera is selected |
| `width`, `height`, `fps` | Source stream dimensions and rounded measured receive fps, not necessarily the consumer's scaled output format; zero when idle after the next status update |
| `mirror`, `rotation` | Current picture options |
| `detail` | Human-readable summary |
| `thermal`, `battery`, `charging` | iPhone thermal state (0 nominal, 1 fair, 2 serious, 3 critical; -1 unknown), battery percent (-1 unknown), charging. Updated every 2 s while live |
| `version` | Windows agent build version (the example is illustrative) |
| `quality` | Selected capture preference: `Auto`, `1080p / 30 fps`, `1080p / 60 fps`, `4K / 30 fps`, `4K / 60 fps`. Actual source dimensions and measured fps remain in `width`, `height`, `fps`; older agents omit this field |

## Timing and connection behavior

`live` is computed from the agent's streaming state and a positive measured frame rate. It is a
sampled indicator, not a per-frame signal: statistics update roughly once a second, and a silent
phone connection is retried after 3 seconds. Use `live`, rather than `state == "live"`, for visibility.
Opening the Tethercam preview window also creates demand and can make `live` true.

The server sends ordinary, unnamed SSE messages with `data: <JSON>` and `retry: 1000` (a one-second
reconnect hint). There are no event IDs or replay history; reconnecting receives a fresh snapshot.
Ignore unknown fields and use `detail` for display only, not machine decisions. Telemetry may retain
the last phone reading after disconnect, so check `live` before treating it as current.

`GET /` aliases `/status`; query strings are ignored. Unknown paths return 404, unsupported methods
return 405, and `OPTIONS` returns 204 with CORS headers. The service is read-only and unauthenticated;
local processes and browser origins allowed by the browser's local-network policy can read it.
It cannot change lenses or camera settings. Browser policy can still block access despite CORS headers.

If port 4790 is occupied, the agent logs a warning and continues without its status service. Check
the agent log when an overlay cannot connect. The overlay should show its offline state on SSE errors.

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

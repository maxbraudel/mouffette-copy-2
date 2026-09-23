# Configuration and storage bootstrap

Only the persisted user settings layer is versioned by storage bootstrap. Environment variables, embedded env files and CLI arguments keep AppConfig precedence and validation. initializePreApplication must not read user settings before bootstrap.

See the [central storage architecture and migration guide](../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.

## Timeline

Maximum duration and slot cadence are copied to each new project. Existing projects retain both saved values. Other timeline settings apply to every project. Editing uses integer slot indices; properties are evaluated at the started slot while videos retain their native cadence and normal speed. The last usable boundary is `floor(maxDurationMs * slotsPerSecond / 1000)`. Configurations containing no complete slot are rejected.

| Environment variable | Default | Allowed range |
| --- | ---: | ---: |
| `MOUFFETTE_TIMELINE_MAX_DURATION_MS` | 180000 | 1–604800000 |
| `MOUFFETTE_TIMELINE_SLOTS_PER_SECOND` | 30 | integer 1–240 |
| `MOUFFETTE_TIMELINE_DEFAULT_CLIP_DURATION_SLOTS` | 30 | integer 1–145152000 |
| `MOUFFETTE_TIMELINE_HEIGHT_PX` | 240 | 120–1200 |
| `MOUFFETTE_CANVAS_MIN_HEIGHT_PERCENT` | 20 | integer 1–98 |
| `MOUFFETTE_TIMELINE_MIN_HEIGHT_PERCENT` | 20 | integer 1–98 |
| `MOUFFETTE_TIMELINE_SPLITTER_HIT_HEIGHT_PX` | 12 | integer 4–48 |
| `MOUFFETTE_TIMELINE_RULER_HEIGHT_PX` | 28 | 16–160 |
| `MOUFFETTE_TIMELINE_CLIP_TRACK_HEIGHT_PX` | 48 | 24–600 |
| `MOUFFETTE_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX` | 8 | integer 1–100 |
| `MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX` | 8 | integer 1–100 |
| `MOUFFETTE_TIMELINE_CLIP_JOINT_MIN_RESIZE_WIDTH_PX` | 24 | integer 0–1000 |
| `MOUFFETTE_TIMELINE_CLIP_MIN_RESIZE_WIDTH_PX` | 24 | integer 0–1000 |
| `MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE` | 10 | integer 0–9999 |
| `MOUFFETTE_TIMELINE_MIN_TRACKS_BELOW` | 10 | integer 0–9999 |
| `MOUFFETTE_TIMELINE_KEYFRAME_SIZE_PX` | 10 | 4–64 |
| `MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT` | 30 | 0–100 |
| `MOUFFETTE_TIMELINE_SNAP_DISTANCE_PX` | 10 | 0–100 |
| `MOUFFETTE_TIMELINE_AUTO_SCROLL_SPEED_PX_PER_SECOND` | 96 | integer 0–2000 |
| `MOUFFETTE_TIMELINE_INITIAL_VIEW_DURATION_MS` | 15000 | 1–604800000 |

The two minimum height percentages must sum to less than 100. They constrain the
expanded canvas/timeline split; collapsing the timeline retains only its transport
row. The draggable split begins at 50/50, clamped to these limits. Its grab area
is centered on the separator and measured in logical pixels.

Production inherits these values unless explicitly overridden in `.env.production`. Shift snapping uses screen pixels, so its tolerance stays consistent at every zoom level.

Clip edge hit zones keep their configured width in logical viewport pixels at
every zoom level. Below the independently configured minimum displayed clip
width, the clip body and its ordinary edge zones move the clip instead of resizing it.
A separate joint handle is centered on each exact clip junction. The ordinary
handles sit immediately to its left and right, retaining their full configured
width with no overlap between the three zones. The widths are independent; the
joint width may equal or exceed the ordinary width. When the junction disappears,
the ordinary handles return to their 50/50 placement on the clip edges.
The joint minimum is measured against the **combined displayed width of both clips**,
independently of the ordinary per-clip minimum. Setting either minimum to 0 disables
that cutoff. The mode of an ongoing
gesture does not change when crossing the threshold. These settings are read at
startup: rebuild after editing the embedded `client/.env`, or restart with
`--env-file <path>` to load an external file without rebuilding.

Clip dragging scrolls at viewport edges at the configured speed in logical pixels
per second, horizontally and vertically. The default is two 48-pixel tracks per
second; 0 disables automatic scrolling. Delayed frames have bounded catch-up to
avoid sudden jumps. The clip body uses an open hand on hover and a closed hand while
moving, like the canvas; trim handles retain their horizontal resize cursor.

The minimum track counts retain empty tracks above and below Track 0, including in
empty projects. The defaults keep 21 tracks, labeled +10 through -10. Occupied
edges still extend the range to leave an empty insertion track; deleting clips
never shrinks the range below these configured minima. Setting both values to 0
restores the occupied range with its insertion tracks, or just Track 0 when empty.

The default clip duration applies when creating images or texts in any project,
and is capped at the space remaining after the playhead. It is measured in the project's slots
(30 slots last one second at 30 slots/s). Existing clips keep their saved length;
videos use their source duration.

## Network recovery diagnostics and retained media

| Variable | Default | Accepted range |
|---|---:|---|
| `MOUFFETTE_REMOTE_MEDIA_RETENTION_MS` | 600000 | 1000–86400000 |
| `MOUFFETTE_REMOTE_MEDIA_CACHE_MAX_MIB` | 10240 | 1–1048576 |
| `MOUFFETTE_NETWORK_DIAGNOSTICS` | true | boolean |
| `MOUFFETTE_NETWORK_DIAGNOSTICS_VERBOSE` | false | boolean |

Transport recovery timing is server authority, advertised by protocol v12 policy
v5. The client cannot renew its fixed session deadline with a local retry.
Diagnostics are profile scoped, rate limited and written by a bounded worker.


## Adaptive screen preview

Screen preview settings follow normal AppConfig precedence: compiled defaults,
selected `.env`, process environment, CLI, then any explicit production override.
They are not persisted user preferences. For example,
`--screen-max-fps=15` overrides `MOUFFETTE_SCREEN_MAX_FPS`. Rebuild after editing
the embedded `client/.env`; an external `--env-file <path>` is re-read on the next
application start without rebuilding. These settings are not watched live.
Invalid settings reject the complete reload, retaining the last valid config.

Bitrate settings describe the **aggregate publisher video budget**, including all
screens and each active publication layer once in v2 (all subscriber copies with a legacy server). They use decimal kilobits per second
(1 kbps = 1000 bits/s), not a per-screen allowance. The upload budget limits the
preview while file transfers need capacity. Budgets are targets for compressed
video admission; they are not guarantees of available bandwidth or wire traffic,
which also includes TLS/TCP headers and retransmissions. Session and control
recovery deadlines remain server authority.

| Environment variable | Default | Allowed range |
| --- | ---: | --- |
| `MOUFFETTE_SCREEN_ADAPTIVE_ENABLED` | true | boolean |
| `MOUFFETTE_SCREEN_LOW_ENABLED` | true | boolean |
| `MOUFFETTE_SCREEN_LOW_MAX_EDGE` | 960 | integer 160–1920 (even) |
| `MOUFFETTE_SCREEN_LOW_MAX_FPS` | 20 | integer 1–60 |
| `MOUFFETTE_SCREEN_LOW_MAX_BITRATE_KBPS` | 750 | integer 32–10000 |
| `MOUFFETTE_SCREEN_LOW_MIN_TOTAL_BITRATE_KBPS` | 600 | integer 64–100000 |
| `MOUFFETTE_SCREEN_MAX_EDGE` | 3840 | integer 320–3840 (even) |
| `MOUFFETTE_SCREEN_MAX_FPS` | 30 | integer 1–60 |
| `MOUFFETTE_SCREEN_IDLE_INTERVAL_MS` | 1000 | integer 250–4000 |
| `MOUFFETTE_SCREEN_MIN_BITRATE_KBPS` | 128 | integer 32–10000 |
| `MOUFFETTE_SCREEN_INITIAL_BITRATE_KBPS` | 1200 | integer 32–100000 |
| `MOUFFETTE_SCREEN_MAX_BITRATE_KBPS` | 12000 | integer 32–100000 |
| `MOUFFETTE_SCREEN_UPLOAD_BITRATE_KBPS` | 1000 | integer 32–100000 |
| `MOUFFETTE_SCREEN_FEEDBACK_INTERVAL_MS` | 500 | integer 100–2000 |
| `MOUFFETTE_SCREEN_RECOVERY_HOLD_MS` | 5000 | integer 1000–60000 |
| `MOUFFETTE_SCREEN_QUEUE_TARGET_MS` | 150 | integer 50–1000 |
| `MOUFFETTE_SCREEN_ACK_TIMEOUT_MS` | 3000 | integer 1000–15000 |
| `MOUFFETTE_SCREEN_KEYFRAME_INTERVAL_MS` | 4000 | integer 500–10000 |
| `MOUFFETTE_SCREEN_MAX_BUFFERED_KIB` | 2048 | integer 32–8192 |
| `MOUFFETTE_SCREEN_MAX_INFLIGHT_FRAMES` | 64 | integer 1–256 |
| `MOUFFETTE_SCREEN_DECODE_QUEUE_MS` | 200 | integer 50–2000 |
| `MOUFFETTE_SCREEN_VIEWPORT_DEBOUNCE_MS` | 200 | integer 50–2000 |
| `MOUFFETTE_SCREEN_VIEWPORT_OVERSAMPLE_PERCENT` | 125 | integer 100–200 |
| `MOUFFETTE_SCREEN_RETRY_INITIAL_MS` | 500 | integer 100–10000 |
| `MOUFFETTE_SCREEN_RETRY_MAX_MS` | 10000 | integer 100–60000 |
| `MOUFFETTE_SCREEN_FIRST_FRAME_TIMEOUT_MS` | 15000 | integer 1000–60000 |
| `MOUFFETTE_SCREEN_STALE_TIMEOUT_MS` | 8000 | integer 2000–60000 |
| `MOUFFETTE_SCREEN_SOFTWARE_PRESET` | veryfast | ultrafast, superfast, veryfast, faster, fast |

`MAX_EDGE` measures the longest encoded edge in pixels, and `MAX_FPS` measures
frames per second. `VIEWPORT_OVERSAMPLE_PERCENT` is the requested pixel margin
above the viewer's displayed size. `MAX_BUFFERED_KIB` uses 1024-byte units per
video socket; `MAX_INFLIGHT_FRAMES` counts pending video receipts. These are
hard safety caps, not a desired queue depth. The sender derives its ordinary
byte allowance from its current video budget, baseline receipt RTT and
`QUEUE_TARGET_MS`, subject to the byte cap; the duration guard remains active.
A larger cap accommodates data in transit on high-bandwidth or long-RTT paths
without authorizing an equally long waiting queue. All `_MS`
settings use milliseconds. The software preset applies to the CPU encoder;
faster presets reduce CPU work at a compression cost.

Validation requires `MIN_BITRATE_KBPS <= INITIAL_BITRATE_KBPS <= MAX_BITRATE_KBPS`
and `MIN_BITRATE_KBPS <= UPLOAD_BITRATE_KBPS <= MAX_BITRATE_KBPS`,
`QUEUE_TARGET_MS < ACK_TIMEOUT_MS`, `RETRY_INITIAL_MS <= RETRY_MAX_MS`, and
`2 * IDLE_INTERVAL_MS <= STALE_TIMEOUT_MS`. Lowering video limits cannot extend a
session lease or grant screen-sharing consent.


## Network proxies

The same proxy policy applies to control, upload and screen sockets before each
connection. Reconnection preserves this policy. TLS certificate verification is
unchanged; use a valid certificate or the system's trusted enterprise CA.

| Environment variable | Default | Allowed values |
| --- | --- | --- |
| `MOUFFETTE_PROXY_TYPE` | system | system, none, http, socks5 |
| `MOUFFETTE_PROXY_HOST` | empty | hostname or IP address, required for http/socks5 |
| `MOUFFETTE_PROXY_PORT` | 8080 | integer 1–65535 |
| `MOUFFETTE_PROXY_USER` | empty | optional explicit-proxy username |
| `MOUFFETTE_PROXY_PASSWORD` | empty | optional explicit-proxy password; requires username |

`system` delegates discovery to Qt's platform proxy support. `none` connects
directly. `http` uses an HTTP CONNECT tunnel, including for a `wss://` server;
it does not mean a separate TLS connection to the proxy. `socks5` supports an
optional username/password. Explicit proxy authentication is supplied only to a
challenge from the configured host and port, never to an unrelated discovered
proxy. System mode uses system proxy credentials where Qt supplies them;
explicit credentials require http/socks5 mode. PAC and integrated enterprise
authentication depend on the platform and deployment and require actual testing.

Keep credentials out of this repository and CLI history: set them through the
process environment or a private external env file. AppConfig marks username
and password sensitive, never includes their content in its validation errors,
and does not export them to network diagnostics. Spaces in a process password
are preserved; use quoting in env files. Control characters are rejected.
SOCKS5 credentials are bounded to 255 UTF-8 bytes each.

References: [Qt proxy factory](https://doc.qt.io/qt-6/qnetworkproxyfactory.html),
[Qt proxy types](https://doc.qt.io/qt-6/qnetworkproxy.html),
[Qt WebSocket authentication](https://doc.qt.io/qt-6/qwebsocket.html#proxyAuthenticationRequired).

The optional low publication layer has its own size, FPS and bitrate caps, but remains part of the existing aggregate source video budget. `MOUFFETTE_SCREEN_LOW_MIN_TOTAL_BITRATE_KBPS` is an activation threshold, not a reserved bitrate. The runtime bounds low quality to the current main profile and suppresses redundant or locally expensive encoding. These settings do not create server-side transcoding.

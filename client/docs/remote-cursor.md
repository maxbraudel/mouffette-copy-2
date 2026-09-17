# Remote cursor

The canvas displays the mouse of its connected target, including during scene
playback when editing is locked. The marker defaults to 30 logical view pixels across;
its center follows the canvas camera but its size does not shrink with zoom.

Configure its diameter in `client/.env` with
`MOUFFETTE_REMOTE_CURSOR_DIAMETER_PX=30` (integer 4..256). This is a local
display setting on the viewing client. Rebuild after changing the embedded
`.env`, or restart with `--env-file /absolute/path/to/client/.env` to load it
from disk without rebuilding. The setting is read at startup.

## Regression and restored path

The v4/v5 session migration removed the former screen-watch/cursor transport.
`cursor_update` was explicitly rejected by both server and client, while
`RemoteCursor.qml` and the canvas cursor state survived. The remaining QML
loader also tied cursor visibility to editing permission.

The replacement is an additive v5 session message, `remote_session_cursor`:

1. `SystemMonitor` samples the target's desktop mouse position.
2. `ApplicationRuntime` publishes only for active incoming remote sessions.
3. `WebSocketClient` stamps the current protocol envelope and session generation.
4. The server validates the authenticated target, exact active session binding,
   increasing sequence and coordinates within the advertised screen, then
   relays only to that session's owner.
5. The owner's client validates the returned binding and sequence, and routes
   the sample to the correct workspace's canvas document.
6. A dedicated `remoteCursorChanged` notification updates the QML marker without
   invalidating the canvas's media, screen, camera or editing projections.

Both clients and the server need this update. The retired `cursor_update` and
screen-watch messages remain rejected; no legacy relay is re-enabled.

## Coordinates and freshness

The payload carries `remoteSessionId`, `generation`, `sequence`, `visible`,
`screenId`, `x` and `y`. Coordinates are relative to the identified screen and
use the same units as its advertised dimensions: physical pixels on macOS and
Windows, Qt screen coordinates on Linux. Hidden samples use screen ID -1 and
zero coordinates. Screen identity removes ambiguity when scaled desktop
rectangles overlap on a mixed-DPI layout.

Windows uses `GetPhysicalCursorPos` with the last captured native inventory.
macOS scales the cursor's logical screen-local position using the same backing
scale as that inventory. Mouse ticks never enumerate displays; periodic capture
and system events replace the shared mapping before publishing new samples. Screen geometry is built from immutable rectangle components:
mutating `QRect::setX/setY` before scaling width/height previously distorted
non-primary Retina screens.

Sampling runs at 16 ms only while an incoming session is active. Unchanged
positions produce a freshness pulse once per second. Sender and relay discard
samples when their socket backlog exceeds 64 KiB, so movement does not add an
unbounded delayed trail. Every new sample carries an increasing sequence;
session generation changes permit a fresh sequence.

The receiving canvas hides its marker when the session becomes inactive or
disconnects, and after 3 seconds without an accepted sample (checked every
500 ms). A fresh pulse restores a stationary marker. Changed session topologies clear the previous marker before installing the new
mapping; the next sample restores it. An unknown screen or out-of-bounds position
hides it. Explicit
stream closure clears the sample so later screen changes cannot resurrect it.
Cursor state is transient and is never saved into the project.

## Regression coverage

- `ScreenCoordinateMapping`: negative origins, Retina geometry, fractional
  scaling and screen-local positions on mixed-DPI desktops.
- `CanvasSelectionBackend`: screen identity, exact edges, invalid samples,
  topology updates, isolated notifications, locked editing, zoom and rendered
  marker pixels.
- `ConnectionManager` and `ClientConnectionFlow`: client wire validation and
  runtime session/stream lifecycle.
- `MediaOverlay`: editor controls unload during test/remote playback while
  the same remote cursor remains visible, including at doubled UI scale.
- Server `remote_session_protocol.test.js`: recipient isolation, authorization,
  bounds, sequence ordering, resumed generations and congestion recovery.

Local validation used the macOS Debug build and the full server test command.
The cursor's rendered-pixel test passed in a native window. General interaction
suites ran with `QT_QPA_PLATFORM=offscreen` because native test windows could not
consistently acquire focus. The existing video-fade and font-metric checks that
failed under that plugin passed when rerun with native Cocoa rendering. Windows
sampling was reviewed against the native API contract but not executed locally.

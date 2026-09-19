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
| `MOUFFETTE_TIMELINE_RULER_HEIGHT_PX` | 28 | 16–160 |
| `MOUFFETTE_TIMELINE_CLIP_TRACK_HEIGHT_PX` | 48 | 24–600 |
| `MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE` | 10 | integer 0–9999 |
| `MOUFFETTE_TIMELINE_MIN_TRACKS_BELOW` | 10 | integer 0–9999 |
| `MOUFFETTE_TIMELINE_KEYFRAME_SIZE_PX` | 10 | 4–64 |
| `MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT` | 30 | 0–100 |
| `MOUFFETTE_TIMELINE_SNAP_DISTANCE_PX` | 10 | 0–100 |
| `MOUFFETTE_TIMELINE_AUTO_SCROLL_SPEED_PX_PER_SECOND` | 96 | integer 0–2000 |
| `MOUFFETTE_TIMELINE_INITIAL_VIEW_DURATION_MS` | 15000 | 1–604800000 |

Production inherits these values unless explicitly overridden in `.env.production`. Shift snapping uses screen pixels, so its tolerance stays consistent at every zoom level.

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

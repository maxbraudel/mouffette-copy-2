# Configuration and storage bootstrap

Only the persisted user settings layer is versioned by storage bootstrap. Environment variables, embedded env files and CLI arguments keep AppConfig precedence and validation. initializePreApplication must not read user settings before bootstrap.

See the [central storage architecture and migration guide](../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.

## Timeline

The maximum duration is copied to each new project. Existing projects retain their saved duration. Other timeline settings apply to every project. Editing uses milliseconds with no temporal grid; these settings never determine rendering frame rate.

| Environment variable | Default | Allowed range |
| --- | ---: | ---: |
| `MOUFFETTE_TIMELINE_MAX_DURATION_MS` | 180000 | 1–604800000 |
| `MOUFFETTE_TIMELINE_HEIGHT_PX` | 240 | 120–1200 |
| `MOUFFETTE_TIMELINE_RULER_HEIGHT_PX` | 28 | 16–160 |
| `MOUFFETTE_TIMELINE_CLIP_TRACK_HEIGHT_PX` | 64 | 24–600 |
| `MOUFFETTE_TIMELINE_KEYFRAME_SIZE_PX` | 10 | 4–64 |
| `MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT` | 30 | 0–100 |
| `MOUFFETTE_TIMELINE_SNAP_DISTANCE_PX` | 10 | 0–100 |
| `MOUFFETTE_TIMELINE_INITIAL_VIEW_DURATION_MS` | 15000 | 1–604800000 |

Production inherits these values unless explicitly overridden in `.env.production`. Shift snapping uses screen pixels, so its tolerance stays consistent at every zoom level.

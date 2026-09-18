# Scene timeline

The scene timeline is the only local preview and animation clock. A remote launch
is a separate action and always starts at zero. Keys, clip positions and lengths, and Stop are stored as integer slot indices.
The project cadence defaults to 30 slots/s; boundary `n` is exactly `n / cadence` seconds. The renderer advances from a monotonic
clock independently of video position notifications.

## State and evaluation

`SceneTimeline` defines element states, keyframes, shared presence clips and scene
timing. Its pure C++ evaluator is shared by `QuickCanvasHost` and
`RemoteSceneController`. Document authoring state, the uncaptured element draft,
the evaluated presentation and the transport/player state are separate. Seeking
and playback never emit document changes or trigger autosave.

Each key captures every intrinsic visual/audio property, excluding media identity,
loading and video position. Numeric geometry, opacity, volume, colours and text
style values interpolate linearly between slot indices and hold until the next slot.
For opacity keys at `x:0` and `x+1:1` the change is immediate at `x+1`; with the
second key at `x+2`, slot `x+1` holds `0.5`. Text, font, visibility, mute, alignment
and effect activation change at the key instant. Effective overrides are resolved
before interpolation and materialized when capturing an intermediate state.
Captured geometry governs playback; text auto-sizing is an editing operation.

Without keys, normal edits change the saved static state. With keys, edits create
a visible draft until captured or updated. Seeking, changing the primary media or
starting playback abandons that draft. The first key holds before its instant,
and the last holds afterwards. Deleting the last key keeps the currently displayed
state as the new static state. Keys at an occupied instant replace that key.

## Clips and transport

Every image, text and video instance owns exactly one presence `clip`, an integer
`trackIndex`, and its absolute scene keyframes. Clips on a track do not overlap.
The first track renders in front; `z` is derived at presentation time and is not
an authoring or keyframe property. Empty intermediate tracks are retained. The
editor derives one trailing empty track from the last occupied index, or presents
one empty track for an empty project.

A new image or text receives one clip from zero to the scene maximum, regardless
of the head or Stop. A new video uses its source duration, rounded up to the grid
and capped at the scene maximum. New instances use the first track free over
their complete interval. Video imports remain pending until metadata or decoded
residency supplies a valid duration; no instance with an empty clip is published.

Clips define half-open presence intervals `[startSlot, endSlot)`. Outside its clip,
the instance is absent from rendering, canvas picking, selection chrome and
overlays. Its clip remains selectable in the timeline without seeking. Runtime
`clipActive` is separate from intrinsic visibility/opacity and is never captured
in a key or saved in the project.

Moving, extending and pasting overwrite only the arrival interval on the target
track. A covered instance is removed; one remaining fragment retains its identity;
two remaining fragments retain the original at the left and create a new instance
at the right. Splitting also creates two independent instances. Every fragment
copies all absolute keyframes, including keys outside its presence interval, so
boundary interpolation is preserved. Source offsets advance for right fragments.
Deletion removes the instance; shortening leaves a gap. Movement retains duration
and is clamped to scene bounds. Keys never follow a clip's temporal displacement.

Document edits validate their entire resulting graph before publishing it. New
source associations are established before removed instances release theirs.
The scene retains its 512-instance and 8 MiB payload limits; a rejected edit has
no partial effect. Timeline clipboard snapshots contain the full instance and
source path, stay scoped to their project, and create new identities at the head
on the active track. Paste truncates at scene end. A lane click selects the target
track; without one the trailing empty track is used. Canvas paste keeps the copied
times and moves the group to a new block below existing tracks, preserving gaps
and relative order; cross-project canvas paste requires matching cadence.

`sourceStartSlot` is null for static clips and a signed grid offset for video.
Video plays at its native cadence and normal speed. Extending a trimmed clip first
recovers available source frames; beyond the source it holds the first/last image
silently. Cuts can produce entirely frozen fragments. Shading shows both
held regions and updates during resize, including the final partial source slot:
250 ms at 30 slots/s occupies 8 slots, with 16⅔ ms of silent frozen video.
Only the source image freezes; independent keyframe animation continues.
Audio is audible only while advancing inside the real source portion of an active
clip, subject to evaluated mute/volume. Pausing, gaps and holds silence the device
without modifying saved audio properties.

Each instance owns an independent `ResidentVideoPlayer` over the shared source.
Both renderers evaluate its source sample from the common scene clock and prepare
the clip entry frame before activation. Paused, absent and held samples stay silent.

Play resumes at the head; at or past the effective end it restarts at zero. Pause
returns to the beginning of the started slot, silently reseeks video and unlocks authoring.
Local preview also runs on an empty project. Space toggles Play/Pause from the
canvas or timeline, except while entering text.
Manual placement chooses the nearest slot (ties go forward). An optional Stop marker defines the
playback end; without it the project maximum is the end. Editing past Stop remains
possible. At Stop, local preview holds the final evaluated state silently. Remote
playback closes through the existing scene/resource release lifecycle.

## Editor and synchronization

The timeline stays below the canvas during preparation, preview and remote
playback. Its transport row shows the current time and frame on the left, centered
playback buttons, and the project maximum time and frame on the right. Both readouts
use monospace digits padded to the project maximum, independently of the Stop marker.
Editing actions align left and zoom/fit align right within one scrollable row.
The gap between them shrinks as the window narrows. If the full labels no longer
fit, all buttons in the row switch to icons with tooltips. If the icons still do
not fit, the entire row scrolls together. Its side padding belongs to the scrollable content;
overflow adds neither a scrollbar nor extra height.
The default panel height is 240 px. A fixed 32 px keyframe band remains above
48 px clip tracks. The ruler and keys remain fixed vertically while the clips
scroll. Every clip displays its media name and supports temporal and vertical
dragging with edge auto-scroll. Clip rows retain their identities when selection
changes. The horizontal scrollbar overlays content without reserving a gutter.
Vertical wheel motion over clips scrolls tracks; horizontal motion or Shift+wheel
scrolls time. Ctrl/Cmd+wheel zooms.
The ruler, zoom, horizontal scroll and fit command navigate the project.
Shift temporarily snaps against all keys and clip boundaries;
releasing it immediately restores ordinary grid alignment. The ruler groups grid
lines when zoomed out; it never changes the actual slot size. Left/right arrows
move one slot when the timeline has focus; the transport displays the current slot.
Clip extensions shade their silent holds. Other media keys are decorative.
All clips are selectable and editable; only the primary instance
exposes editable keyframes. Canvas group copy/delete remain available. Clipboard and delete commands are routed by focus between text,
canvas and timeline. A canvas paste between projects requires matching cadence;
an animation is never silently reinterpreted on a different grid.

Remote launch preserves preparation, first-image verification, a synchronized
activation barrier and an immutable revision. Every media is prepared, including
media initially outside screens or clips. Initially absent videos prepare their
clip entry frame without displaying it. Screen intersections follow evaluated geometry.
Periodic snapshots carry only the continuous timeline time (including fractional
milliseconds and the position within a slot), never presentation
properties, and cannot overwrite animated states. Display loss hides the affected
output while the clock continues; returning screens resume the current state.
See [window presentation](window-presentation.md).

## Configuration and formats

`AppConfig` exposes the nine `MOUFFETTE_TIMELINE_*` settings documented in the
[configuration registry](../src/backend/config/README.md). Maximum duration is
captured in each new project (default 180000 ms), together with cadence
(`MOUFFETTE_TIMELINE_SLOTS_PER_SECOND`, integer 1–240, default 30); visual settings apply globally.
The initial viewport spans 15000 ms and is independent of playback cadence.

Project component version 8 has an explicit reset barrier from version 7. Older
projects also reset directly to version 8; other profile components and external
source files are preserved. Render schema 6 and wire protocol 11 require a
coordinated client/server rollout. Older render and clipboard formats are rejected.
Client and server validate clip cardinality, global identities, non-overlapping
intervals within each track, source intervals, track indices and payload limits.

## Sources panel

The overlay lists only referenced image/video sources, grouped by SHA-256 identity
with canonical paths as a temporary identity while hashing. It excludes text and
has no instance selection behavior. Its rows keep the existing transfer controls,
progress and metadata, but report native dimensions and sort by source name.
Transfer/cache state is authoritative per source and destination. Fragmentation
and duplication do not unload or upload again; removing the last usage reconciles
the destination inventory without affecting other workspaces using that source.

Regression coverage: `SceneTimeline`, `TimelineController`,
`CanvasSelectionBackend`, `VideoPlaybackBackend`, `RemoteSceneLifecycle`,
`RemoteSceneControllerLifecycle`, `RemoteSessionIntegration`, storage tests and
server timeline/protocol tests. Native backend checks must run on each target OS.

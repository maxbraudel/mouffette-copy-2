# Scene timeline

The scene timeline is the only local preview and animation clock. A remote launch
is a separate action and always starts at zero. Keys, clip positions and lengths, and Stop are stored as integer slot indices.
The project cadence defaults to 30 slots/s; boundary `n` is exactly `n / cadence` seconds. The renderer advances from a monotonic
clock independently of video position notifications.

## State and evaluation

`SceneTimeline` defines element states, keyframes, source video clips and scene
timing. Its pure C++ evaluator is shared by `QuickCanvasHost` and
`RemoteSceneController`. Document authoring state, the uncaptured element draft,
the evaluated presentation and the transport/player state are separate. Seeking
and playback never emit document changes or trigger autosave.

Each key captures every intrinsic visual/audio property, excluding media identity,
loading and video position. Numeric geometry, opacity, volume, colours and text
style values interpolate linearly between slot indices and hold until the next slot.
For opacity keys at `x:0` and `x+1:1` the change is immediate at `x+1`; with the
second key at `x+2`, slot `x+1` holds `0.5`. Text, font, visibility, mute, alignment, order
and effect activation change at the key instant. Effective overrides are resolved
before interpolation and materialized when capturing an intermediate state.
Captured geometry governs playback; text auto-sizing is an editing operation.

Without keys, normal edits change the saved static state. With keys, edits create
a visible draft until captured or updated. Seeking, changing the primary media or
starting playback abandons that draft. The first key holds before its instant,
and the last holds afterwards. Deleting the last key keeps the currently displayed
state as the new static state. Keys at an occupied instant replace that key.

## Clips and transport

Clips refer to source intervals without producing new files. They play at normal
speed and native video cadence. Source offsets also use the project grid, not native
video frame numbers. Source length rounds up once: 250 ms at 30 slots/s occupies
8 slots (266⅔ ms), ending with 16⅔ ms of silent frozen video. Only a fragment
reaching the actual source end contains this compensation. Splitting cannot add
more compensation; extension cannot exceed the last slot covering the source.
A move preserves occupied duration and stays within project bounds. Pasting starts
at the head and truncates at the project end. Insertion, movement and extension
overwrite only the arrival interval, retaining correctly offset source fragments.
Trimming shorter or deleting leaves a gap. Keys and clips remain independent.

Before the first clip, show its source entry image. In a gap or after the last
clip, hold the preceding exit image. An empty track shows source time zero.
Audio is audible only while the timeline advances inside an active clip, subject
to evaluated mute and volume. Pausing and seeking silence the device without
altering saved audio properties.

`TimelineVideoPlayback` translates the evaluated source sample into asynchronous
`ResidentVideoPlayer` seeks and normal playback. Pending seeks coalesce, stale
responses are ignored and the last valid image remains visible. Contiguous source
cuts do not cause another seek. Both renderers use the same synchronization policy.

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
The gap between them shrinks as the window narrows; once the buttons no longer fit,
the entire row scrolls together. Its side padding belongs to the scrollable content;
overflow adds neither a scrollbar nor extra height.
The ruler, zoom, horizontal scroll and fit command navigate the project.
Shift temporarily snaps against all keys and clip boundaries;
releasing it immediately restores ordinary grid alignment. The ruler groups grid
lines when zoomed out; it never changes the actual slot size. Left/right arrows
move one slot when the timeline has focus; the transport displays the current slot.
Clip tails show their silent compensation. Other media keys are decorative.
Only the explicitly selected primary media is editable; canvas group copy/delete
remain available. Clipboard and delete commands are routed by focus between text,
canvas and timeline. A canvas paste between projects requires matching cadence;
an animation is never silently reinterpreted on a different grid.

Remote launch preserves preparation, first-image verification, a synchronized
activation barrier and an immutable revision. Every media is prepared, including
media initially outside screens. Screen intersections follow evaluated geometry.
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

Project component version 6 replaces versions 1–5 through the explicit bootstrap
reset transition. Other profile components are preserved. Render schema 4 and
wire protocol 9 require a coordinated client/server rollout. Legacy automation,
video ranges and older render payloads are rejected, not converted. Timeline
validation applies on both client and server, including time bounds, strict
property schemas, source intervals and existing payload size limits.

Regression coverage: `SceneTimeline`, `TimelineController`,
`CanvasSelectionBackend`, `VideoPlaybackBackend`, `RemoteSceneLifecycle`,
`RemoteSceneControllerLifecycle`, `RemoteSessionIntegration`, storage tests and
server timeline/protocol tests. Native backend checks must run on each target OS.

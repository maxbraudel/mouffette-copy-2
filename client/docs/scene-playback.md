# Scene timeline

The scene timeline is the only local preview and animation clock. A remote launch
is a separate action and always starts at zero. Time is stored as integer
milliseconds; there is no placement grid. The renderer advances from a monotonic
clock independently of video position notifications.

## State and evaluation

`SceneTimeline` defines element states, keyframes, source video clips and scene
timing. Its pure C++ evaluator is shared by `QuickCanvasHost` and
`RemoteSceneController`. Document authoring state, the uncaptured element draft,
the evaluated presentation and the transport/player state are separate. Seeking
and playback never emit document changes or trigger autosave.

Each key captures every intrinsic visual/audio property, excluding media identity,
loading and video position. Numeric geometry, opacity, volume, colours and text
style values interpolate linearly. Text, font, visibility, mute, alignment, order
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
speed. A move preserves duration and stays within project bounds. Pasting starts
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
retains the head and unlocks authoring. An optional Stop marker defines the
playback end; without it the project maximum is the end. Editing past Stop remains
possible. At Stop, local preview holds the final evaluated state silently. Remote
playback closes through the existing scene/resource release lifecycle.

## Editor and synchronization

The timeline stays below the canvas during preparation, preview and remote
playback. Its ruler, precise time field, zoom, horizontal scroll and fit command
navigate the project. Shift temporarily snaps against all keys and clip boundaries;
releasing it immediately restores free placement. Other media keys are decorative.
Only the explicitly selected primary media is editable; canvas group copy/delete
remain available. Clipboard and delete commands are routed by focus between text,
canvas and timeline.

Remote launch preserves preparation, first-image verification, a synchronized
activation barrier and an immutable revision. Every media is prepared, including
media initially outside screens. Screen intersections follow evaluated geometry.
Periodic snapshots carry only the current timeline time, never presentation
properties, and cannot overwrite animated states. Display loss hides the affected
output while the clock continues; returning screens resume the current state.
See [window presentation](window-presentation.md).

## Configuration and formats

`AppConfig` exposes the eight `MOUFFETTE_TIMELINE_*` settings documented in the
[configuration registry](../src/backend/config/README.md). Maximum duration is
captured in each new project (default 180000 ms); visual settings apply globally.
The initial viewport spans 15000 ms and is independent of playback cadence.

Project component version 5 replaces version 4 through the explicit bootstrap
reset transition. Other profile components are preserved. Render schema 3 and
wire protocol 8 require a coordinated client/server rollout. Legacy automation,
video ranges and older render payloads are rejected, not converted. Timeline
validation applies on both client and server, including time bounds, strict
property schemas, source intervals and existing payload size limits.

Regression coverage: `SceneTimeline`, `TimelineController`,
`CanvasSelectionBackend`, `VideoPlaybackBackend`, `RemoteSceneLifecycle`,
`RemoteSceneControllerLifecycle`, `RemoteSessionIntegration`, storage tests and
server timeline/protocol tests. Native backend checks must run on each target OS.

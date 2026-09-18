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

Every image, text and video has a track of non-overlapping `Clip` intervals.
A newly created image or text receives one clip from zero to the scene maximum,
regardless of the head or Stop. A new video uses its source duration, rounded up
to the grid and capped at the scene maximum. `clipsInitialized` prevents deleted
clips from being regenerated when a file is restored or its residency changes.
Import metadata supplies the video duration before full decoding, so a loading
skeleton has the same clip presence as the eventual content, even while waiting
for memory admission.

Clips define half-open presence intervals `[startSlot, endSlot)`. Outside all clips,
including an empty track, the element is absent from both renderers and from
canvas picking, selection chrome and overlays. It remains selectable in the media
list and editable in the inspector/timeline. Runtime `clipActive` is separate from
intrinsic visibility/opacity and is never captured in a key or saved in the project.

All clips use the same editing operations and QML component. Movement preserves
duration and stays within scene bounds. Insertion, movement and extension overwrite
only the arrival interval, keeping source-correct fragments on either side.
Shortening or deletion leaves a gap. Keys retain their absolute scene positions.
Static insertion fills the gap from the head to the next clip or scene maximum;
it is disabled at an occupied head. Video insertion adds the full source from the
head, truncated at the scene end. Clipboard clips remain scoped to their project
and media; paste truncates at the scene end.

`sourceStartSlot` is null for static clips and a signed grid offset for video.
Video plays at its native cadence and normal speed. Extending a trimmed clip first
recovers available source frames; beyond the source it holds the first/last image
silently. Cuts can produce entirely frozen fragments. Diagonal hatching shows both
held regions and updates during resize, including the final partial source slot:
250 ms at 30 slots/s occupies 8 slots, with 16⅔ ms of silent frozen video.
Only the source image freezes; independent keyframe animation continues.
Audio is audible only while advancing inside the real source portion of an active
clip, subject to evaluated mute/volume. Pausing, gaps and holds silence the device
without modifying saved audio properties.

`TimelineVideoPlayback` translates the evaluated source sample into asynchronous
`ResidentVideoPlayer` seeks and normal playback. Pending seeks coalesce, stale
responses are ignored and the last valid image remains visible within an active clip. Contiguous source
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
The gap between them shrinks as the window narrows. If the full labels no longer
fit, all buttons in the row switch to icons with tooltips. If the icons still do
not fit, the entire row scrolls together. Its side padding belongs to the scrollable content;
overflow adds neither a scrollbar nor extra height.
The tracks fill the panel to its bottom edge; their horizontal scrollbar overlays
the content without reserving a gutter.
The ruler, zoom, horizontal scroll and fit command navigate the project.
Shift temporarily snaps against all keys and clip boundaries;
releasing it immediately restores ordinary grid alignment. The ruler groups grid
lines when zoomed out; it never changes the actual slot size. Left/right arrows
move one slot when the timeline has focus; the transport displays the current slot.
Clip extensions show their silent holds with diagonal hatching. Other media keys are decorative.
Only the explicitly selected primary media is editable; canvas group copy/delete
remain available. Clipboard and delete commands are routed by focus between text,
canvas and timeline. A canvas paste between projects requires matching cadence;
an animation is never silently reinterpreted on a different grid.

Remote launch preserves preparation, first-image verification, a synchronized
activation barrier and an immutable revision. Every media is prepared, including
media initially outside screens or clips. Initially absent videos prepare their
first source frame without displaying it. Screen intersections follow evaluated geometry.
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

Project component version 7 migrates version 6 at bootstrap, preserving projects,
keys and video clips and adding full-scene clips to existing images/text. Versions
1–5 retain their explicit reset policy. Other profile components are preserved.
Render schema 5 and wire protocol 10 require a coordinated client/server rollout. Legacy automation,
video ranges and older render payloads are rejected, not converted. Timeline
validation applies on both client and server, including time bounds, strict
property schemas, source intervals and existing payload size limits.

Regression coverage: `SceneTimeline`, `TimelineController`,
`CanvasSelectionBackend`, `VideoPlaybackBackend`, `RemoteSceneLifecycle`,
`RemoteSceneControllerLifecycle`, `RemoteSessionIntegration`, storage tests and
server timeline/protocol tests. Native backend checks must run on each target OS.

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
editor retains a minimum range from Track +10 through Track -10, including in
empty projects. `MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE` and
`MOUFFETTE_TIMELINE_MIN_TRACKS_BELOW` configure these nonnegative counts independently.
One empty insertion row remains above the first occupied track and below the last,
extending beyond the minimum range when necessary. Track 0 remains the
fixed origin: positive labels extend upwards, negative labels downwards. Saved
`trackIndex` coordinates grow downwards, so the displayed number is `-trackIndex`.
The visible range includes zero, interior gaps and the two insertion rows, bounded
by stored indices -9999 and 9999. Adding or pasting above existing clips never
renumbers them. Existing schema-6 projects keep their indices and layer order
without migration. Both clients and the relay must support signed indices for
remote scenes containing tracks above zero; older validators reject those scenes.
Edits beyond either track limit are rejected atomically.

A new image, text or video starts at the playhead's current slot. Pending imports
retain the slot captured when the file was added, including across project reloads.
Images and texts use `MOUFFETTE_TIMELINE_DEFAULT_CLIP_DURATION_SLOTS` slots
(default 30); videos use their source duration, rounded up to the grid.
The duration is capped at the space remaining before the scene maximum,
independently of Stop. At the final boundary no new instance can be added.
Existing clips retain their saved timing. Each new canvas instance gets a new
track above all existing instances, even when an existing track has temporal space.
Video imports remain pending until metadata or decoded residency supplies a valid
duration; they are inserted above existing tracks when ready. No instance with an
empty clip is published.

Clips define half-open presence intervals `[startSlot, endSlot)`. Outside its clip,
the instance is absent from rendering, canvas picking, selection chrome and
overlays. Its clip remains selectable in the timeline without seeking. Runtime
`clipActive` is separate from intrinsic visibility/opacity and is never captured
in a key or saved in the project.

Moving and extending avoid other clips by default. Horizontal movement stops at
the nearest neighbour in either direction, including fast pointer jumps across a
whole clip. A track change is accepted only if the whole clip fits at the requested
time; otherwise the last valid placement remains. At a junction between two clips
on the same track, pressing the dedicated handle centered on their junction rolls the
shared boundary: one clip grows
by exactly the amount the other shrinks, with both outer edges fixed and at least
one slot remaining on each side. Both clips preview together and commit atomically;
video source offsets follow the changed start edge, while keyframes stay fixed.
The ordinary handle on either side resizes only its own clip, allowing a gap
to open without changing its neighbour. An independent extension stops at contact.
The pointer location at press chooses the mode for the whole gesture. The backend
also requires an explicit rolling mode; ordinary trim commands never compensate a neighbour.
Preview resolution does not mutate the document; the commit validates again.

Holding the physical Control key enables overwrite for moving and resizing,
including at a shared boundary instead of rolling the two clips together.
Pressing/releasing it refreshes the preview immediately, including with a stationary
pointer; releasing it over an overlap restores an allowed placement. On macOS this
is Control, not Command (Qt maps physical Control to `MetaModifier`/`Key_Meta`).
Shift snapping remains independent, and collision constraints take priority.
Timeline paste retains overwrite without requiring this modifier.

Overwrite affects only the arrival interval on the target track. A covered
instance is removed; one remaining fragment retains its identity;
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
on the active track. Paste truncates at scene end. After a successful clip paste,
the head advances to the new clip's actual end and scrolls into view, allowing
successive pastes to append clips. Keyframe pastes keep the current position.
A lane click selects the target
track; without one the leading empty row is used. Canvas paste keeps the copied
times and moves the group to a new block above existing tracks, preserving gaps
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
During remote preparation, playback and stopping, the playhead line and cap use
the disabled-button gray. Ruler dragging and all seek commands are blocked until
the remote scene ends; the playhead continues to follow the scene's position.
Manual navigation is also locked: wheel/trackpad scrolling, scrollbars, zoom and
fit controls remain disabled until the scene ends. Automatic playhead following
continues during remote playback.
Editing actions align left and zoom/fit align right within one scrollable row.
The gap between them shrinks as the window narrows. If the full labels no longer
fit, all buttons in the row switch to icons with tooltips. If the icons still do
not fit, the entire row scrolls together. Its side padding belongs to the scrollable content;
overflow adds neither a scrollbar nor extra height.
The canvas and timeline each occupy half of the available page height. The
Timeline toggle beside Settings hides everything below the transport row and
returns that space to the canvas; timing readouts and Start/Play/End remain visible.
The Timeline toggle stays visible and usable during local and remote playback;
Settings, Selection and Text controls are unloaded while editing is locked.
Reopening preserves horizontal zoom and scroll, and the vertical center.
A fixed 32 px keyframe band remains above
48 px clip tracks. The ruler and keys remain fixed vertically while the clips
scroll. A fixed column to the left of the time viewport displays Keyframes and
Track 0 with positive and negative neighbours. Its shared width measures every track name with the rendered
font, plus 12 px padding on each side and a 1 px separator. Clip track names follow
the clips' vertical scroll; the Keyframes header stays fixed. Header clicks are
informational and wheel events navigate the same timeline. Track backgrounds do
not highlight the active paste destination. Clips fill the entire track height
without vertical insets. Only the media name is shown, centered in the intersection
of its clip with the visible time viewport, including during drag and resize.
Long names elide to the available width. Invisible resize zones straddle edges
equally inside and outside the clip when no joint handle is present, with a horizontal double-arrow cursor. Each
zone stays a fixed number of logical viewport pixels wide, independently of time
zoom or clip duration (`MOUFFETTE_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX`, default 8).
Below `MOUFFETTE_TIMELINE_CLIP_MIN_RESIZE_WIDTH_PX` pixels of displayed clip width
(default 24; 0 disables this threshold), the clip body and both ordinary edge
zones move the clip instead, using the hand cursor. Zooming in restores resizing.
An edit already in progress keeps its original mode when crossing this threshold.
At a junction, a separate handle is centered on the exact shared boundary.
Its width is configured by `MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX`
(default 8), independently of the ordinary handle width. It uses the horizontal
split cursor, with arrows and a vertical divider, for rolling both clips. The
ordinary right handle of the left clip sits immediately before the joint handle;
the ordinary left handle of the right clip sits immediately after it. Each retains
its full width, without overlapping the joint handle. Opening a gap restores their
50/50 placement on the clip edges immediately, including during the preview.
The joint handle has its own cutoff, `MOUFFETTE_TIMELINE_CLIP_JOINT_MIN_RESIZE_WIDTH_PX`
(default 24; 0 disables it), measured against the **sum of the two displayed clip
widths**. Individual resizing still uses the separate per-clip cutoff. An ongoing
joint gesture retains its handle and mode even when one clip becomes very short.
These dimensions remain fixed in viewport pixels. Cursor feedback
follows the chosen mode throughout the drag, including outside the viewport;
holding Control temporarily uses the independent overwrite cursor.
While rolling, Shift snapping excludes the clip boundaries of both edited clips,
including their original shared boundary. Other clips, stationary keyframes and
the playhead remain snap targets.
Overlapping zones belong to the nearest clip body, independently of selection
or stacking order; exact ties go to the clip on the right. Clip bodies use an
open hand on hover and the canvas's closed hand while moving. Clips support
temporal and vertical dragging with edge auto-scroll at 96 logical px/s by default,
configured by `MOUFFETTE_TIMELINE_AUTO_SCROLL_SPEED_PX_PER_SECOND` (0 disables it).
The clip viewport initially centers
Track 0. Track coordinates and the viewport center stay fixed when the range grows
at either end; resizing preserves the center and removal clamps to the remaining
range. Selecting a partly visible clip never scrolls it under the pointer. The
active paste row follows the selected clip's identity. Clip delegates retain
their identities when selection changes. The horizontal scrollbar overlays content without reserving a gutter.
Vertical wheel motion over clips scrolls tracks; horizontal motion or Shift+wheel
scrolls time. Ctrl/Cmd+wheel zooms.
The ruler, zoom, horizontal scroll and fit command navigate the project.
Ruler clicks/drags, Start/End and slot navigation preserve instance and keyframe
selection, including when the selected instance is absent at the new playhead.
Clicking empty clip-track content clears instance selection and chooses the paste
destination. Clicking empty keyframe content only clears the selected keyframe;
the selected instances and their displayed keys remain. The header column does neither.
Shift temporarily snaps against all keys and clip boundaries, including when
scrubbing the playhead on the ruler. Moving or resizing a clip also snaps its edges
to the playhead, using the same pixel threshold and guide; the ruler never snaps to
its own playhead position. Pressing or releasing Shift updates a held drag
even without pointer movement. Releasing it restores ordinary grid alignment. The ruler groups grid
lines when zoomed out; it never changes the actual slot size. Left/right arrows
move one slot when the timeline has focus; the transport displays the current slot.
Shift+Left/Right jumps to the nearest clip start or end strictly in that direction
across all tracks, including tracks outside the viewport. Coincident boundaries
count as one position; keyframes and Stop are not targets. With no further boundary
the head stays put. Jumps preserve selection and scroll horizontally to reveal the head.
Snap targets and clip offsets are resolved in integer slots. A clip's guide is
published only after placement constraints confirm that the chosen edge reaches
the target; rounding in millisecond display values cannot erase a valid guide.
Coincident targets prefer the playhead label when clip-to-playhead snapping is
enabled. Guide labels stay within the visible time viewport and render names as
plain text, including at either edge and after horizontal scrolling.
Clip extensions shade their silent holds. Other media keys are decorative.
All clips are selectable and editable. `CanvasDocument` owns instance selection;
clip highlighting, stacking and the primary clip ID are projections of that same
selection. Clearing clip selection from the timeline also clears it in the canvas.
Only the primary instance exposes editable keyframes. Keyframe selection is
independent: reselecting or moving the same clip preserves its selected keyframe;
changing the primary instance clears keys belonging to the previous instance.
Timeline copy/delete target the last selected area (clips or keyframes).
Deleting a keyframe keeps the instance selected, including after the final key;
an empty keyframe selection never falls back to copying or deleting the clip.
Canvas group copy/delete remain available. Clipboard and delete commands are routed by focus between text,
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

`AppConfig` exposes the `MOUFFETTE_TIMELINE_*` settings documented in the
[configuration registry](../src/backend/config/README.md). Maximum duration is
captured in each new project (default 180000 ms), together with cadence
(`MOUFFETTE_TIMELINE_SLOTS_PER_SECOND`, integer 1–240, default 30); visual settings apply globally.
The initial viewport spans 15000 ms and is independent of playback cadence.

Project component version 8 has an explicit reset barrier from version 7. Older
projects also reset directly to version 8; other profile components and external
source files are preserved. Render schema 6 and wire protocol 12 require a
coordinated client/server rollout. Older render and clipboard formats are rejected.
Client and server validate clip cardinality, global identities, non-overlapping
intervals within each track, source intervals, track indices and payload limits.

## Sources panel

The entire overlay, including Upload and Launch Remote Scene, is hidden while the
canvas has no media instances. Adding content (including text) shows it; removing
the final instance hides it again. Upload and its separator appear only when an
image or video source is present; text-only canvases retain the remote scene action.

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

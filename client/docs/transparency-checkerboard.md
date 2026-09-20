# Selected-media transparency

The authoring canvas places a semi-transparent checkerboard behind the primary selected
image, video or text rectangle. It remains visible at zero content opacity and
when the eye control hides the content. Secondary selections have no checker.
Deselection removes it. Outside a selected clip's interval, its outline and
checker remain available for movement and resizing, while its content stays
hidden. Geometry holds the first clip slot before its start and the last visible
slot (`endSlot - 1`) at or after its end, including when keyframes lie outside
the clip. Unselected inactive clips remain absent. Existing draft/keyframe
capture rules still apply to geometry edits.

During playback and preparation, selection remains in the document on standby.
All selection visuals (checker, outline, handles and timeline highlights) use the
editing-enabled gate, and selection commands cannot change it. Pause, completion
or a failed launch makes the same selection available again; scrubbing during
playback keeps it on standby until playback actually stops.

By default, opacity is 50% and each cell is 8 logical interface pixels,
including on high-DPI displays. Both are configurable in `client/.env`:

```dotenv
MOUFFETTE_CANVAS_CHECKERBOARD_OPACITY_PERCENT=50
MOUFFETTE_CANVAS_CHECKERBOARD_CELL_SIZE_PX=8
```

Opacity accepts integers from 0 (invisible) to 100 (opaque). Cell size accepts
integers from 1 to 256: smaller cells make the grid denser (4 is twice as many
cells per row as 8); larger cells make it coarser. Invalid values are rejected
at startup. Rebuild after editing the embedded `.env`, or restart with
`--env-file <path>` to use an external file. Process variables and matching CLI
options follow the usual configuration precedence. Production inherits these
settings unless explicitly overridden in `.env.production`.

The grid's origin is the canvas viewport's top-left corner. Pan, media movement,
normal resize, Alt-resize and zoom change the exposed region, never the cell
size or grid phase. Theme roles `transparencyCheckerA/B` supply opaque neutral
grays and update with the application's light/dark palette. Checkerboard opacity
blends those colors over the canvas and any media beneath the selection.

## Rendering and input

`CanvasRoot.qml` keeps the media's geometry and paint order on its delegate.
Only `MediaVisual` inherits authored visibility and opacity. A selected hidden
or fully transparent body remains draggable through the existing input router;
other invisible bodies remain excluded from picking. Text editing ends when
its content becomes invisible and cannot reopen until the content is visible.

A loader creates `TransparencyCheckerboard.qml` only for the primary selection.
The shader draws the media/viewport intersection, with the parent's media and
camera scales canceled. Its geometry and coordinate uniforms are bounded by
the viewport even for million-pixel destinations. The procedural fragment
shader uses one quad, no sampled texture, offscreen layer, timer, per-cell QML
objects or CPU painting. The ordinary Qt Quick GPU backends execute the shader;
Qt Shader Tools builds its portable `.qsb` resource once for the app and tests.

The checker is a sibling behind the media content. Higher media can occlude
both, while transparent selected content exposes its own checker. Loading
surfaces and selection controls keep their existing order. `MediaVisual` and
the passive remote scene renderer do not create a checker, so the feature does
not alter projected output or serialized media properties.

## Verification

`TransparencyCheckerboard` checks GPU readback against the grid at DPR 1 and 2,
both color palettes, fractional bounds, compensated transforms, empty bounds
and inherited UI opacity. `CanvasInteraction` checks production integration:
selection, themes, hidden-body drags, text edit ownership, pan/zoom/live resize,
extreme bounds, source alpha/global opacity, paint order and passive output.

```sh
ctest --test-dir out/build/macos-debug --output-on-failure \
  -R '^(TransparencyCheckerboard|CanvasInteraction(Scaled)?|CanvasSelectionBackend|MediaOverlay(Scaled)?|MediaFrameItem|RemoteSceneLifecycle)$'
```

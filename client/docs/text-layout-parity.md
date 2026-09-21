# Canvas and remote text layout

## Incident and cause

An outlined text fitted on the authoring canvas could wrap its last character
after disabling fit-to-text and starting a remote scene. The JSON contained the
complete text, dimensions, style and fit flag. The receiver changed the layout
inputs when projecting that state into the shared `MediaVisual` / `TextItem`.
This was reproducible without changing operating systems.

For bundled Impact at 64 pixels, `Texte` has a 139.5-pixel advance. With an
outline of 1.875 percent, the raw outline calculation is 1.2 pixels:

| Layout input | Canvas | Previous remote renderer |
| --- | ---: | ---: |
| Effective outline | 1 px | 1.2 px |
| Inset per side: `4 + ceil(outline) + 1` | 6 px | 7 px |
| Fitted outer width | 152 px | 152 px |
| Available text width | 140 px | 138 px |
| Text lines with wrapping enabled | `Texte` | `Text` / `e` |

Fit-to-text uses `NoWrap`, which concealed the narrower content area. Turning
it off enabled normal wrapping and exposed the disagreement. Increasing the
box or forbidding word wrapping would have hidden the inconsistent state.

## Layout contract

- `TextRenderMetrics::outlinePixels` defines effective outline thickness from
  percentage and integer font size. Fit measurement, canvas rendering, scene
  serialization and remote rendering all use it.
- The layout rectangle is the final scene size divided by the element scale.
  It remains fractional until rendering. Remote publication must not round it
  or use redundant serialized base dimensions differently from the canvas.
- Font size and weight are rounded identically before rendering interpolated
  timeline states. Outline thickness uses that same rounded font size.
- Fit measurement uses `QTextDocument`, like Qt Quick `TextEdit`, with shared
  design metrics, zero document margin and trailing-space options. Plain-text
  paragraph separators, empty paragraphs and fallback glyphs are therefore
  interpreted by the same layout implementation.
- The fitted rectangle encloses the measured content plus the outline inset.
  There is no one-pixel tolerance when applying it: a fraction of a pixel of
  additional text advance can require the next enclosing pixel.
- Disabling fit keeps the authored rectangle. It changes the wrap mode and
  must not implicitly alter the effective style or content margins.

The document change also fixes `U+2029` paragraph separators, which the old
manual newline splitting did not count as separate paragraphs. Removing the
size tolerance fixes edits such as `Texte` to `Textg` at 22 pixels: the required
outer width grows from 56 to 57 pixels, even though the text advance changes
by less than one pixel.

## Font investigation and scope

Production registers the bundled `resources/fonts/impact.ttf` before creating
media. Text regression suites explicitly load that file too. On the investigated
Mac, registration selected the bundled Impact 2.35 despite an installed Impact
5.00x with the same family name. Both versions measured `Texte` identically.
Independent local CoreText and Qt FreeType probes of the bundled face also
returned the same 139.5-pixel advance at 64 pixels.

These probes rule out a necessary font-engine change for this reproduction;
they are not a Windows execution. The general font contract still uses family
names: an arbitrary unbundled family or a character missing from Impact can
resolve to different fallback fonts on different machines. Guaranteeing those
cases would require font assets and fallback policy, beyond this geometry fix.

## Regression coverage

- `RemoteSceneControllerLifecycle`: real canvas/remote text documents after
  disabling fit, five outline thicknesses at two outer scales; all projected
  text fields and fractional geometry at interpolated timeline positions.
- `TextItemQml` and `TextItemQmlScaled`: fitted dimensions and unchanged line
  counts after toggling fit, including whitespace, Unicode separators, empty
  text, fallback glyphs, styles and outlines.
- `CanvasSelectionBackend`: refitting one missing pixel, preserving the
  alignment anchor, and the `Texte` to `Textg` content-edit boundary.

Both clients need the corrected build for parity in either authoring direction.
The payload shape is unchanged; no server or stored-project migration is needed.

## Validation on 2026-09-21

The Development application and affected Qt test targets compiled with Qt
6.11.2 on macOS. Complete suites passed: `TextOutlineItem`, `TextOutlineMotion`,
`TextItemQml`, `TextItemQmlScaled`, `SceneTimeline`,
`RemoteSceneControllerLifecycle` and `QmlArchitectureGate`.

The new canvas fit regressions passed independently. Twenty-eight text editing,
caret placement and resize cases passed using Qt's offscreen platform to avoid
native window activation and screen-size constraints.

Regression sensitivity was checked by temporarily restoring each old behavior:
the outlined remote text test failed with two lines versus one; the one-pixel
content edit failed with width 56 instead of 57; the Unicode paragraph test
failed with insufficient fitted height. Corrected sources were restored and
the application rebuilt afterward.

The broader native `CanvasInteraction` suite encountered window-activation
failures; `CanvasSelectionBackend` was stopped after repeated failures waiting
for window activation. `CanvasInteractionScaled` encountered the five caret
fixtures that click beyond the available window, previously documented in
[the outline investigation](text-outline-rendering.md). These full native suites
are not reported as passing. Windows was not executed locally.

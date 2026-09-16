# Application theme

[`Theme.qml`](../resources/qml/app/Theme.qml) owns the application's UI colors.
It follows the active Qt system palette through reactive QML bindings; switching
the OS appearance does not require reopening a page, canvas, or popup.

## Semantic roles

| Role | Use |
| --- | --- |
| `windowBackground` | The system base color and main application background |
| `surfaceBackground` | Panels and history cards |
| `elevatedBackground` | Floating panels, dialogs and canvas controls; always opaque |
| `recessedBackground` | Canvas workspace and input wells |
| `text`, `mutedText`, `disabledText` | Primary, secondary and unavailable content |
| `border`, `controlBorder`, `focusBorder` | Separators, input outlines and keyboard focus |
| `accent`, `onAccent` | Blue actions and text/icons placed on a solid blue fill |
| `selectionBackground`, `selectionText` | Text selection, including canvas text editing |
| `connected*`, `warning*`, `error*`, `scene*` | Shared green, amber, red and purple state colors |
| `canvas*`, `selection*`, `snapGuide`, `uiZone*` | Editor screens, selection handles, guides and system zones |
| `overlay*`, `slider*`, `media*` | Canvas floating controls and media transfer indicators |
| `controlPalette` | The same colors exposed to Qt Quick Controls |

Legacy names such as `brandBlue` and `mediaUploaded` are semantic aliases, not
separate palettes. Status foregrounds use distinct light/dark values. Their
translucent backgrounds use common pigments, preserving the meaning of the
status without relying on a single foreground color on every surface.

Secondary text is mixed from the foreground and background to keep readable
contrast. Do not use `SystemPalette.mid` for text: that role describes a bevel
or border and can be almost invisible against a dark background.

## Layering and bindings

Floating surfaces must have an opaque neutral base. `ToastStack` paints
`toastBackground` first, then the translucent severity tint, then its border and
text. Status badges and actions can use a translucent tint when their parent
already supplies the opaque base. Do not lower a whole panel's opacity to create
a background tint: that also fades its labels and controls.

Use declarative bindings, for example `color: Theme.overlayText`. Do not copy
theme values into component initialization handlers or cache them in C++ models;
those copies do not update when the palette changes. Screen zone models publish
their semantic type, with QML choosing the theme color.

Canvas primitives use `import Mouffette.App as AppStyle` and
`AppStyle.Theme.*`. The qualified import keeps sibling canvas types resolved
locally, both in the packaged application and in tests that load QML from source.

The SVG icon shapes remain unchanged. Canvas controls recolor their icon layer
from the theme rather than displaying the SVG's embedded light foreground.

## Content boundaries

The theme styles the editor, not authored content. Media text, outlines,
highlights and color swatches retain the project's colors. Remote scene output
remains transparent where required. The remote pointer keeps a white fill and
dark outline so it remains recognizable on arbitrary media. SVG/masking source
colors and transparent rectangles are not independent UI palettes.

## Verification

`MediaOverlay` exercises the production QML while changing the application
palette in both directions. It checks readable secondary/status colors, canvas
and overlay updates, and the toast's opaque base plus translucent tint.
`MediaOverlayScaled`, `CanvasInteraction`, `CanvasInteractionScaled`, and
`TextItemQml` / `TextItemQmlScaled` cover overlays, pointer behavior and text editing at normal and
scaled display densities.

```sh
ctest --test-dir out/build/macos-debug --output-on-failure \
  -R '^(MediaOverlay|CanvasInteraction|TextItemQml)(Scaled)?$'
```

Use the corresponding configured build directory on other platforms.

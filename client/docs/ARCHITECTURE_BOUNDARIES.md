# Client presentation boundaries

## Allowed directions

- QML consumes presentation controllers and `QAbstractListModel` instances.
- Presentation controllers call domain services and documentary canvas APIs.
- Backend domain, network, upload, file, and persistence code does not locate
  or mutate QML controls.
- Platform window helpers accept `QWindow`.

## Forbidden application UI

- QWidget windows, pages, controls, dialogs, or overlays;
- `QQuickWidget` embedding;
- QGraphics scene/view/item/effect rendering;
- QSS and stylesheet mutation;
- C++ `findChild` access to QML controls;
- legacy/fallback renderer selection.

The tray is the sole native Widgets exception. Run
`./tools/check_architecture_boundaries.sh` or CTest's
`QmlArchitectureGate` to enforce these rules.

## State-dependent labels

Controls and badges with changing action/status text reserve their widest
variant from the first frame. `AppButton` and `OverlayActionButton` expose
`textVariants`; other labels use `StateTextMetrics.maximumWidth` in their
implicit sizing. The helper uses Qt Quick `FontMetrics` with the rendered font,
and recomputes when that font or the variants change. Keep state variants next
to the displayed text and update both when adding a new state. Existing fixed
layout slots (icon buttons and inspector range buttons) already retain width.

Connection badges share `ConnectionStatusMetrics`. Upload progress reserves
its counter's digit capacity from the media count before the transfer begins,
including the separate monospace font. Do not cache the greatest width seen so
far: that still allows the first transition to move the layout.

## Storage compatibility

All persisted-component versions and migration/reset policies are centralized
in [runtime/storage](../src/backend/runtime/storage/README.md). Business stores
read and write their current formats; bootstrap runs before their consumers.
Compatibility decisions and migration code must not be added to QML, domain
loaders or individual application controllers.

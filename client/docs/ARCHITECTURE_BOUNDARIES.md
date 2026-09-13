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

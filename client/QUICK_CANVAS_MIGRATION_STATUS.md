# Qt Quick/QML migration status

Status: implementation complete in the client source tree.

- Application shell and every application-owned surface: QML.
- Local canvas: `CanvasRoot.qml` backed by `CanvasDocument` and typed
  view-models.
- Remote receiver: `RemoteSceneWindow.qml` in direct `QQuickWindow` instances.
- Theme: centralized, palette-aware `Theme` singleton with light/dark updates.
- Native exception: tray integration only (`QApplication` and
  `QSystemTrayIcon`).
- Legacy UI: removed, including MainWindow/UI file, widget pages and overlays,
  hidden QGraphics canvas, renderer selector, and fallback path.
- Build: QML module and application build successfully on the local macOS Qt
  6.11 toolchain.
- Verification: native CTest suite, 1×/2× status-card/media-overlay pixel
  tests, application smoke launch, deterministic input contracts, and the
  QML-only architecture gate.

Windows compilation, packaging, and multi-monitor E2E remain platform release
validation activities and must run on their native CI/host; no Windows result
is implied by the macOS verification above.

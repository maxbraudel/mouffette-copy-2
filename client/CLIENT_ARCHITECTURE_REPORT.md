# Mouffette Client — Qt Quick architecture

The client has one presentation stack: Qt Quick/QML. There is no alternate
widget renderer and no runtime renderer selection.

## Composition

`src/main.cpp` creates `QApplication`, one `QQmlApplicationEngine`, and the
non-visual `ApplicationController`. `QApplication` is retained only for the
native `QSystemTrayIcon` integration. The engine loads `Mouffette.App/Main`.

`ApplicationController` is the presentation composition root. It exposes
typed application state, list models, dialogs, settings, navigation commands,
and the active `CanvasSessionViewModel`. It does not own or locate visual QML
items.

`ApplicationRuntime` coordinates networking, sessions, uploads, files,
persistence, system lifecycle, and the tray. It has no widget pointers.

## QML presentation

`resources/qml/app/` owns the application window, bootstrap, top bar, Clients,
Canvas, History, settings, dialogs, toasts, and shared controls. `Theme.qml` is
the single token source for light/dark colors, typography, radii, dimensions,
and spacing.

`CanvasRoot.qml` is the only local canvas surface. It consumes
`QuickCanvasController` properties and commands through
`CanvasSessionViewModel`. Media, selection chrome, guides, input routing,
overlays, the Scene/Element panel, and the media action panel all live in the
same Qt Quick tree.

`RemoteSceneWindow.qml` is instantiated as a `QQuickWindow` for each target
screen. Platform helpers operate on `QWindow`; media playback remains in C++
through `QMediaPlayer`, `QVideoSink`, and `QAudioOutput` runtimes.

## State ownership

- `CanvasDocument` owns media, screens, camera, selection, Z order, and
  serialization without depending on a scene graph.
- `CanvasMedia` is the stable documentary media entity and owns only
  non-visual media runtime state.
- `QuickCanvasController` projects document state to QML and applies typed
  interaction commands. It never searches, reparents, positions, or styles a
  QML control.
- `MediaSettingsViewModel` validates and translates settings; QML never edits
  domain objects directly.
- `ClientListModel`, `SceneActivityListModel`, `HistoryListModel`,
  `ToastListModel`, and `MediaListModel` are the list authorities consumed by
  delegates.

## Enforced boundary

Application UI source may not contain `QWidget`, `QMainWindow`, `QQuickWidget`,
QGraphics scene/view/item/effect types, QSS, `findChild`, or `setStyleSheet`.
`QuickWidgets`, `MultimediaWidgets`, and `SvgWidgets` are not linked. The only
Widgets usage is `QApplication` plus the isolated native tray service.

The boundary is executable: `QmlArchitectureGate` runs from CTest and
`tools/check_architecture_boundaries.sh` runs the same rule set in CI.

# Client Qt Quick/QML migration

This document records the completed architectural decision. All Mouffette
application surfaces use Qt Quick/QML; C++ remains responsible for domain,
network, persistence, media runtime, and native system integration.

The migration preserved the existing structure, wording, colors, dimensions,
responsive thresholds, and light/dark behavior. It did not retain a hybrid
release mode or a fallback renderer.

Implemented scope:

- QML bootstrap, `ApplicationWindow`, top bar, navigation, Clients, Canvas,
  History, settings, confirmations, information dialogs, and toasts;
- a shared `SegmentedStatusCard` for local and remote status, using full-height
  segments and a single overlaid border without masks or effects;
- one `CanvasRoot.qml`, including tools, Scene/Element settings, media actions,
  guides, selection chrome, playback controls, and the bottom-right panel;
- `CanvasDocument`, `CanvasMedia`, typed list models, canvas and media settings
  view-models;
- direct `QQuickWindow` remote scene windows and `QWindow` platform helpers;
- one `qt_add_qml_module`, no QuickWidgets/MultimediaWidgets/SvgWidgets, no
  renderer flag, no QWidget/QGraphics application UI path;
- Qt Quick component, pixel, input, document, upload, session, persistence,
  remote lifecycle, and architecture-gate tests.

Release policy is a single final Qt Quick path. A regression is handled by
reverting the offending change, not by reviving the removed renderer.

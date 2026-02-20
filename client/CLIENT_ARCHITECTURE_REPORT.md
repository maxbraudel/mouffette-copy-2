# Mouffette Client — Architecture Report

## Scope and conventions

This report documents the **client application codebase** (Qt/C++), with a focus on maintainable source files.

- Included: app source, resources, build/run scripts, UI definitions.
- Excluded from detailed analysis: generated artifacts (`build/`), OS metadata (`.DS_Store`), and third-party vendor internals (`vendor/skia`).
- Goal: for each file, explain its role and context in the app.

---

## Client file tree with per-file role

## Top-level files

- `CMakeLists.txt` — Main build definition; declares sources/headers, Qt modules, platform-specific files, and link settings.
- `Info.plist` — macOS bundle metadata and app identity configuration.
- `README.md` — Build/run and feature notes for the client.
- `build.sh` — Unix shell build helper wrapping CMake workflow.
- `build.ps1` — PowerShell build helper for Windows.
- `run.sh` — Unix shell run helper.
- `run.ps1` — PowerShell run helper for Windows.
- `update_includes_frontend_backend.sh` — Utility script to update include paths after frontend/backend refactors.

## Resources

- `resources/resources.qrc` — Qt resource registry bundling icons and image assets into the executable.
- `resources/mouffette.png` — Main image asset (branding/app visual).
- `resources/icons/arrow-down.svg` — UI icon (arrow down control).
- `resources/icons/arrow-up.svg` — UI icon (arrow up control).
- `resources/icons/delete.svg` — UI icon for delete action.
- `resources/icons/loop.svg` — UI icon for loop toggle/action.
- `resources/icons/pause.svg` — Media pause icon.
- `resources/icons/play.svg` — Media play icon.
- `resources/icons/settings.svg` — Settings icon used in overlays/toolbars.
- `resources/icons/stop.svg` — Stop icon.
- `resources/icons/visibility-off.svg` — Visibility toggle OFF icon.
- `resources/icons/visibility-on.svg` — Visibility toggle ON icon.
- `resources/icons/volume-off.svg` — Mute icon.
- `resources/icons/volume-on.svg` — Volume icon.
- `resources/icons/text/fit-to-text.svg` — Text tool icon for fit-to-content behavior.
- `resources/icons/text/horizontal-align-left.svg` — Text horizontal alignment left icon.
- `resources/icons/text/horizontal-align-center.svg` — Text horizontal alignment center icon.
- `resources/icons/text/horizontal-align-right.svg` — Text horizontal alignment right icon.
- `resources/icons/text/vertical-align-top.svg` — Text vertical alignment top icon.
- `resources/icons/text/vertical-align-center.svg` — Text vertical alignment center icon.
- `resources/icons/text/vertical-align-bottom.svg` — Text vertical alignment bottom icon.
- `resources/icons/tools/selection-tool.svg` — Canvas selection mode icon.
- `resources/icons/tools/text-tool.svg` — Canvas text mode icon.

## UI definition

- `ui/MainWindow.ui` — Designer UI artifact (legacy/compatibility); runtime UI is largely programmatic in `MainWindow`.

## Source root

- `src/main.cpp` — Application entrypoint: app setup, cache cleanup hooks, `MainWindow` creation, lifecycle wiring.
- `src/MainWindow.h` — Main orchestrator declaration; central dependency holder, state accessors, and event slots.
- `src/MainWindow.cpp` — Main composition root implementation; initializes managers/handlers/controllers/pages and routes high-level app flow.

## Backend controllers

- `src/backend/controllers/CanvasSessionController.h` — API for per-client canvas session lifecycle/orchestration.
- `src/backend/controllers/CanvasSessionController.cpp` — Creates/switches/configures canvas sessions, handles upload-button/session linkage, and session-level cleanup.
- `src/backend/controllers/TimerController.h` — Timer orchestration interface (reconnect, watched-state sync, cursor sending).
- `src/backend/controllers/TimerController.cpp` — Implements reconnect backoff, periodic state sync, and watched-cursor update timers.

## Backend domain — media

- `src/backend/domain/media/MediaItems.h` — Core media item classes (image/video base abstractions, resize handles, upload states, overlay hooks).
- `src/backend/domain/media/MediaItems.cpp` — Media item behavior implementation: rendering transforms, interactions, overlays, playback-related internals.
- `src/backend/domain/media/TextMediaItem.h` — Text media item type declaration and text-specific settings/behavior.
- `src/backend/domain/media/TextMediaItem.cpp` — Text media implementation: editing, layout, style application, and serialization behavior.

## Backend domain — models

- `src/backend/domain/models/ClientInfo.h` — Core transport/domain model for clients and screens (`ClientInfo`, `ScreenInfo`, `UIZone`).
- `src/backend/domain/models/ClientInfo.cpp` — JSON serialization/deserialization and display helpers for client/screen models.

## Backend domain — session

- `src/backend/domain/session/SessionManager.h` — Session store contract for per-remote canvas sessions and indexed lookups.
- `src/backend/domain/session/SessionManager.cpp` — Session storage/index implementation (persistent client ID, server ID, canvas session ID mappings).

## Backend files

- `src/backend/files/FileManager.h` — High-level file façade combining repository/tracker/cache behaviors.
- `src/backend/files/FileManager.cpp` — Delegates file ID mapping, upload tracking, cache preload/release, and idea-file associations.
- `src/backend/files/FileMemoryCache.h` — In-memory file-bytes cache interface (performance for media replay/loading).
- `src/backend/files/FileMemoryCache.cpp` — Cache lifecycle implementation for fast file access.
- `src/backend/files/FileWatcher.h` — Watches filesystem changes for media source paths.
- `src/backend/files/FileWatcher.cpp` — Emits removal events when source files disappear; keeps canvas consistent with disk state.
- `src/backend/files/LocalFileRepository.h` — File ID ↔ path mapping service interface.
- `src/backend/files/LocalFileRepository.cpp` — Repository implementation for deterministic file identification and lookup.
- `src/backend/files/Theme.h` — Theme-level value definitions/utilities (legacy bridge for style constants).
- `src/backend/files/Theme.cpp` — Theme helper implementation used by UI styling flow.

## Backend handlers

- `src/backend/handlers/WebSocketMessageHandler.h` — Connection/message lifecycle handler API.
- `src/backend/handlers/WebSocketMessageHandler.cpp` — Handles connected/disconnected transitions, reconnect state handling, and `state_sync` application.
- `src/backend/handlers/ScreenEventHandler.h` — Screen/registration event handler API.
- `src/backend/handlers/ScreenEventHandler.cpp` — Syncs local registration/screens/volume, processes incoming remote screen info, and updates active canvas state.
- `src/backend/handlers/ClientListEventHandler.h` — Client list event processing API.
- `src/backend/handlers/ClientListEventHandler.cpp` — Reconciles online/offline clients, active session identity, remote re-ID after reconnects, and list updates.
- `src/backend/handlers/UploadEventHandler.h` — Upload interaction handler API.
- `src/backend/handlers/UploadEventHandler.cpp` — Handles upload button logic, file collection from canvas, dedupe/cleanup, and per-file visual progress updates.

## Backend managers — app

- `src/backend/managers/app/SettingsManager.h` — Persistent settings manager contract.
- `src/backend/managers/app/SettingsManager.cpp` — Loads/saves server URL, auto-upload preference, persistent client ID generation strategy, settings dialog.
- `src/backend/managers/app/SystemTrayManager.h` — System tray integration API.
- `src/backend/managers/app/SystemTrayManager.cpp` — Tray icon/menu setup and tray event signaling.
- `src/backend/managers/app/MenuBarManager.h` — Menu bar manager API.
- `src/backend/managers/app/MenuBarManager.cpp` — Builds menu actions (quit/about) and emits app-level commands.

## Backend managers — network

- `src/backend/managers/network/ClientListBuilder.h` — Client list composition helper API.
- `src/backend/managers/network/ClientListBuilder.cpp` — Merges connected and remembered sessions into display-ready list entries.
- `src/backend/managers/network/ConnectionManager.h` — Connection utility/abstraction API (legacy helper layer around connection policies).
- `src/backend/managers/network/ConnectionManager.cpp` — Connection management utility logic.

## Backend managers — system

- `src/backend/managers/system/SystemMonitor.h` — System metrics provider API (screens/volume/platform).
- `src/backend/managers/system/SystemMonitor.cpp` — Platform-specific screen enumeration and volume monitoring (macOS/Windows).

## Backend network

- `src/backend/network/WebSocketClient.h` — WebSocket protocol client API: control channel, upload channel, message senders, signals.
- `src/backend/network/WebSocketClient.cpp` — Message transport implementation, reconnect strategy, protocol parsing/routing, upload-channel fallback behavior.
- `src/backend/network/UploadManager.h` — Upload state machine API (sender + target sides).
- `src/backend/network/UploadManager.cpp` — Chunked transfer implementation, progress tracking, incoming assembly, cancel/unload/cleanup logic.
- `src/backend/network/WatchManager.h` — Watch/unwatch flow manager API.
- `src/backend/network/WatchManager.cpp` — Manages current watched client target and watch toggle semantics.
- `src/backend/network/RemoteFileTracker.h` — Remote upload state tracking API (which client has which file, per-idea links).
- `src/backend/network/RemoteFileTracker.cpp` — Upload footprint tracking implementation and file removal notifications.

## Backend platform

### macOS

- `src/backend/platform/macos/MacVideoThumbnailer.h` — macOS video thumbnail generation interface.
- `src/backend/platform/macos/MacVideoThumbnailer.mm` — Objective-C++ implementation using native macOS media APIs.
- `src/backend/platform/macos/MacWindowManager.h` — macOS window behavior helper API.
- `src/backend/platform/macos/MacWindowManager.mm` — Objective-C++ window-level behavior (always-on-top/workspace interactions).

### Windows

- `src/backend/platform/windows/WindowsVideoThumbnailer.h` — Windows video thumbnail generation interface.
- `src/backend/platform/windows/WindowsVideoThumbnailer.cpp` — Native thumbnail extraction implementation on Windows.

## Frontend handlers

- `src/frontend/handlers/WindowEventHandler.h` — Window lifecycle handling API.
- `src/frontend/handlers/WindowEventHandler.cpp` — Handles resize/show/hide/close/system-state events, delegates window-specific side effects.
- `src/frontend/handlers/UploadSignalConnector.h` — Upload signal wiring helper API.
- `src/frontend/handlers/UploadSignalConnector.cpp` — Connects upload manager signals to MainWindow/session updates in one place.

## Frontend managers — UI

- `src/frontend/managers/ui/TopBarManager.h` — Local top-bar widget manager contract.
- `src/frontend/managers/ui/TopBarManager.cpp` — Builds and styles local client info block (“You” + connection status).
- `src/frontend/managers/ui/RemoteClientInfoManager.h` — Remote top-bar info manager API.
- `src/frontend/managers/ui/RemoteClientInfoManager.cpp` — Manages remote client name/status/volume container and atomic state application.
- `src/frontend/managers/ui/RemoteClientState.h` — Value object representing remote UI state transitions (connected/connecting/error/etc.).
- `src/frontend/managers/ui/UploadButtonStyleManager.h` — Upload button styling logic API.
- `src/frontend/managers/ui/UploadButtonStyleManager.cpp` — Applies contextual upload button labels/styles/progress state to match manager/session state.

## Frontend rendering — canvas

- `src/frontend/rendering/canvas/ScreenCanvas.h` — Core canvas class declaration; interaction engine, overlays, tools, remote scene controls.
- `src/frontend/rendering/canvas/ScreenCanvas.cpp` — Main canvas implementation (~critical hotspot): screen layout, media manipulation, DnD imports, overlays, cursor mapping, scene launch controls.
- `src/frontend/rendering/canvas/OverlayPanels.h` — Media overlay panel primitives and interactive controls declarations.
- `src/frontend/rendering/canvas/OverlayPanels.cpp` — Overlay widgets implementation (play/pause, sliders, labels, mouse-blocking proxy items, etc.).
- `src/frontend/rendering/canvas/RoundedRectItem.h` — Reusable rounded rectangle graphics item utility.
- `src/frontend/rendering/canvas/SegmentedButtonItem.h` — Segmented button graphics item for canvas tool switching UX.

## Frontend rendering — navigation

- `src/frontend/rendering/navigation/ScreenNavigationManager.h` — Navigation/loader state manager API for list/canvas transitions.
- `src/frontend/rendering/navigation/ScreenNavigationManager.cpp` — Implements transition policy, delayed spinner logic, canvas reveal flow, and reconnect loading behavior.

## Frontend rendering — remote

- `src/frontend/rendering/remote/RemoteSceneController.h` — Remote scene playback controller contract (target-side rendering).
- `src/frontend/rendering/remote/RemoteSceneController.cpp` — Validates incoming scene payloads, builds per-screen windows, instantiates media playback/rendering on remote target.

## Frontend UI — layout

- `src/frontend/ui/layout/ResponsiveLayoutManager.h` — Responsive relocation/visibility policy API.
- `src/frontend/ui/layout/ResponsiveLayoutManager.cpp` — Moves remote-info container and toggles top-bar buttons by viewport width/page context.

## Frontend UI — notifications

- `src/frontend/ui/notifications/ToastNotificationSystem.h` — Toast subsystem API.
- `src/frontend/ui/notifications/ToastNotificationSystem.cpp` — Non-blocking toast implementation for info/success/warning/error events.

## Frontend UI — overlays/canvas

- `src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.h` — Top-level global canvas overlay host API.
- `src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.cpp` — Hosts shared global canvas controls anchored over the viewport.
- `src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.h` — Canvas media settings panel declaration (extracted overlay UI).
- `src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.cpp` — Media settings panel implementation (for selected media editing controls).

## Frontend UI — pages

- `src/frontend/ui/pages/ClientListPage.h` — Client list page API.
- `src/frontend/ui/pages/ClientListPage.cpp` — Renders connected clients and ongoing scenes list, emits selection signals.
- `src/frontend/ui/pages/CanvasViewPage.h` — Canvas view page API (canvas host + top info widgets + fades).
- `src/frontend/ui/pages/CanvasViewPage.cpp` — Builds canvas page container, spinner stack, remote status/volume widgets, and page-level transitions.

## Frontend UI — theme

- `src/frontend/ui/theme/StyleConfig.h` — Central style constants/tokens (sizes/gaps/radii/typography scales).
- `src/frontend/ui/theme/AppColors.h` — Color palette/token declarations.
- `src/frontend/ui/theme/AppColors.cpp` — Color token definitions and helper conversions.
- `src/frontend/ui/theme/ThemeManager.h` — Theming API and style-application helpers.
- `src/frontend/ui/theme/ThemeManager.cpp` — Applies runtime styles to widgets/pages and creates standardized button styles.

## Frontend UI — widgets

- `src/frontend/ui/widgets/ClientListDelegate.h` — Custom item delegate API for client list rendering/separators.
- `src/frontend/ui/widgets/ClientListDelegate.cpp` — Delegate painting implementation.
- `src/frontend/ui/widgets/ClippedContainer.h` — Utility container ensuring style clipping/rounded-border correctness.
- `src/frontend/ui/widgets/ClippedContainer.cpp` — Clipped container implementation.
- `src/frontend/ui/widgets/RoundedContainer.h` — Rounded container widget API.
- `src/frontend/ui/widgets/RoundedContainer.cpp` — Rounded container implementation.
- `src/frontend/ui/widgets/SpinnerWidget.h` — Lightweight spinner widget API.
- `src/frontend/ui/widgets/SpinnerWidget.cpp` — Spinner animation drawing implementation.
- `src/frontend/ui/widgets/QtWaitingSpinner.h` — Alternate waiting spinner API.
- `src/frontend/ui/widgets/QtWaitingSpinner.cpp` — Alternate spinner implementation.

---

## Global structure and runtime flow summary

The client follows a layered but pragmatic architecture. `main.cpp` boots Qt and creates `MainWindow`, which acts as the application composition root: it instantiates networking (`WebSocketClient`), session/file/upload/watch systems, UI managers, event handlers, and page widgets. At runtime, `ScreenNavigationManager` controls transitions between `ClientListPage` and `CanvasViewPage`. When a remote client is selected, `CanvasSessionController` creates/restores a session-scoped `ScreenCanvas`, requests remote screens via WebSocket, and re-establishes watch state. Incoming protocol events are routed by specialized handlers (`WebSocketMessageHandler`, `ScreenEventHandler`, `ClientListEventHandler`, `UploadEventHandler`) that mutate session/UI state. File/media upload logic is handled by `UploadManager` with `FileManager` tracking file IDs, remote upload presence, and per-idea associations. The overall result is a session-aware remote-canvas client where each remote target has an isolated canvas state, upload state, and reconnect behavior.

---

## Deep dive — Canvas page and canvas system

## 1) Canvas page structure (`CanvasViewPage`)

The canvas page is a host shell around the active `ScreenCanvas` and includes page-level UI elements:

- **Remote client info container** (name + connection status + volume)
  - Built in `CanvasViewPage` and mirrored/managed in main top bar via `RemoteClientInfoManager`.
- **Canvas container**
  - Visual bordered area (`#CanvasContainer`) containing a stacked widget.
- **Canvas stack (`m_canvasStack`)**
  - Index 0: fullscreen loader spinner page.
  - Index 1: actual canvas page (`m_canvasHostStack`) that hosts per-session canvases.
- **Canvas host stack (`m_canvasHostStack`)**
  - Holds multiple `ScreenCanvas` instances (one per session); only one visible at a time.
- **Opacity effects + fade animations**
  - Loader fade, canvas fade, and volume fade for smooth state transitions.

Main files:
- `src/frontend/ui/pages/CanvasViewPage.h/.cpp`
- `src/frontend/rendering/navigation/ScreenNavigationManager.h/.cpp`
- `src/frontend/managers/ui/RemoteClientInfoManager.h/.cpp`

## 2) What exists inside `ScreenCanvas` (all canvas elements)

`ScreenCanvas` is the interaction/rendering core and contains:

### A) Base scene layers

- **Screen rectangles** (`m_screenItems`) representing remote displays.
- **Per-screen UI zones** (`m_uiZoneItems`) for dock/taskbar/menu-bar overlays.
- **Remote cursor graphics item** (`m_remoteCursorDot`) mapped from remote global coordinates.
- **Selection chrome**: custom borders + 8 handles around selected media.

### B) Media elements on canvas

- **Image media items** (from `ResizableMediaBase` family in `MediaItems`).
- **Video media items** (`ResizableVideoItem`) with playback/thumbnail support.
- **Text media items** (`TextMediaItem`) with inline editing and text formatting controls.

Each media item supports placement, resize, z-order, selection, and overlay updates.

### C) Global overlays (viewport-level)

- **Media list/info overlay panel** (`m_infoWidget`)
  - Lists all media with name, dimensions, file size/status, and upload progress.
  - Contains control header including upload and scene launch actions.
- **Global settings toggle + global settings panel**
  - Settings button and `CanvasMediaSettingsPanel` to edit selected media properties.
- **Tool selector**
  - Selection tool and Text tool segmented control.
- **Custom floating scrollbar behavior** for overlay content.

### D) Per-media inner overlays (attached to media items)

Defined mainly through `OverlayPanels` + media item internals:

- **Video controls overlay** (play/pause/loop/time/seek interactions depending on media type).
- **Name/info labels** rendered over media where applicable.
- **Show/hide and interaction blockers** via dedicated overlay graphics items.
- **Slider/handle interaction items** using mouse-blocking proxy logic to avoid accidental scene drags.

### E) Canvas interactions

- Drag & drop import (image/video files).
- Pan/zoom with smooth anchor-preserving transforms.
- Snap-to-screen/snap-to-media logic with visual snap guides.
- Selection semantics preserving behavior under overlays.
- Text-tool creation on click.
- Reconnect-preserving content hide/show behavior.

Main files:
- `src/frontend/rendering/canvas/ScreenCanvas.h/.cpp`
- `src/backend/domain/media/MediaItems.h/.cpp`
- `src/backend/domain/media/TextMediaItem.h/.cpp`
- `src/frontend/rendering/canvas/OverlayPanels.h/.cpp`
- `src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.h/.cpp`
- `src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.h/.cpp`

## 3) How canvas is linked to backend

Canvas is tightly linked to backend through explicit connectors:

- **Screens data and watched state**
  - `ScreenEventHandler` receives remote `screens_info` and calls `ScreenCanvas::setScreens(...)` for the active session.
- **Session scoping**
  - `SessionManager` stores per-remote `CanvasSession` containing the session’s canvas pointer and upload metadata.
- **Uploads and file lifecycle**
  - `UploadEventHandler` scans canvas media, builds upload manifest, and calls `UploadManager`.
  - `FileManager` maps media→file IDs and tracks per-client/per-idea upload state.
- **Connection loss/reconnect behavior**
  - `WebSocketMessageHandler` and `ScreenNavigationManager` drive loader/inline reconnect states while preserving canvas context.
- **Remote scene launch/stop**
  - `ScreenCanvas` sends launch/stop commands via `WebSocketClient`.
  - Target-side `RemoteSceneController` validates payload and reconstructs remote scene windows/media.

---

## Canvas-related process files (focused list)

### Core canvas engine

- `src/frontend/rendering/canvas/ScreenCanvas.h` — Declares all canvas systems: scene layers, tools, overlays, media operations, remote-scene controls.
- `src/frontend/rendering/canvas/ScreenCanvas.cpp` — Implements almost all canvas runtime behavior (input, rendering, overlays, media flow, protocol-triggered state).

### Media model and item behavior

- `src/backend/domain/media/MediaItems.h` — Media item class hierarchy and shared interaction APIs.
- `src/backend/domain/media/MediaItems.cpp` — Shared media interaction/rendering/upload state behavior.
- `src/backend/domain/media/TextMediaItem.h` — Specialized text media declaration.
- `src/backend/domain/media/TextMediaItem.cpp` — Specialized text item editing/layout behavior.

### Canvas overlay systems

- `src/frontend/rendering/canvas/OverlayPanels.h` — Overlay item declarations for media controls.
- `src/frontend/rendering/canvas/OverlayPanels.cpp` — Overlay visual/control implementation.
- `src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.h` — Settings panel declaration for selected media.
- `src/frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.cpp` — Settings panel behavior and binding to media state.
- `src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.h` — Global overlay host declaration.
- `src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.cpp` — Placement/lifecycle of shared global canvas controls.

### Page host and navigation around canvas

- `src/frontend/ui/pages/CanvasViewPage.h` — Canvas page shell declaration.
- `src/frontend/ui/pages/CanvasViewPage.cpp` — Canvas host stack + spinner stack + remote info UI implementation.
- `src/frontend/rendering/navigation/ScreenNavigationManager.h` — Canvas/list transition API.
- `src/frontend/rendering/navigation/ScreenNavigationManager.cpp` — Loader orchestration and reveal logic for canvas availability/reconnect.

### Session and orchestration

- `src/backend/controllers/CanvasSessionController.h` — Session-scoped canvas orchestration API.
- `src/backend/controllers/CanvasSessionController.cpp` — Creates/configures/switches session canvases and keeps upload button/session state synchronized.
- `src/backend/domain/session/SessionManager.h` — Session storage model contract.
- `src/backend/domain/session/SessionManager.cpp` — Session store and indices (persistent/server/canvas session IDs).

### Data ingress and outbound protocol

- `src/backend/handlers/ScreenEventHandler.h` — API for incoming remote screen updates.
- `src/backend/handlers/ScreenEventHandler.cpp` — Converts server screen payloads into active session canvas updates.
- `src/backend/handlers/ClientListEventHandler.h` — Client list-driven state transitions API.
- `src/backend/handlers/ClientListEventHandler.cpp` — Handles remote availability transitions and canvas reconnect targeting.
- `src/backend/handlers/WebSocketMessageHandler.h` — WebSocket lifecycle handler API.
- `src/backend/handlers/WebSocketMessageHandler.cpp` — Connection lifecycle and state synchronization impacting canvas availability.
- `src/backend/network/WebSocketClient.h` — Protocol send/receive API.
- `src/backend/network/WebSocketClient.cpp` — Transport and protocol parsing implementation.

### Upload/file linkage used by canvas

- `src/backend/handlers/UploadEventHandler.h` — Upload UI click handling API.
- `src/backend/handlers/UploadEventHandler.cpp` — Builds upload batches from canvas media and updates per-item upload visuals.
- `src/backend/network/UploadManager.h` — Upload state machine API.
- `src/backend/network/UploadManager.cpp` — Upload protocol implementation (chunking/assembly/progress/unload).
- `src/backend/files/FileManager.h` — File tracking façade API.
- `src/backend/files/FileManager.cpp` — File ID tracking and remote upload-state book-keeping implementation.
- `src/backend/files/FileWatcher.h` — Media source file watcher API.
- `src/backend/files/FileWatcher.cpp` — Deletes/refreshes canvas media when source files disappear.

### Remote target reconstruction (remote scene)

- `src/frontend/rendering/remote/RemoteSceneController.h` — Target-side scene reconstruction API.
- `src/frontend/rendering/remote/RemoteSceneController.cpp` — Validates received scene payloads and reconstructs windows/media for remote rendering.

---

## Practical mental model (short)

Think of the canvas feature as a **session-scoped rendering engine** (`ScreenCanvas`) hosted inside `CanvasViewPage`, with `MainWindow` as orchestrator. Session identity and state are owned by `SessionManager`; incoming/outgoing network events are handled by specialized handlers and `WebSocketClient`; upload/file consistency is governed by `UploadManager` + `FileManager`; and remote-scene execution is completed by `RemoteSceneController` on the target side. This gives each remote client its own persistent canvas state, while still supporting reconnect, re-watch, upload/unload, and synchronized scene launch flows.

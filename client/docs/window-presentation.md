# Window presentation and remote scene priority

`WindowPresentation` owns the Qt Quick shell's opening geometry.
`WindowStackingCoordinator` owns the shared native stacking policy: active remote
scene surfaces stay above the control window and its dialogs/menus. Startup after bootstrap, tray activation and a second process's
activation request all use the same `open()` path.

- The native title bar, system menu and minimize/maximize/close controls are
  explicitly preserved alongside the topmost hint. Qt's Windows backend does
  not supply its default decoration hints once extra window flags are set;
  using only `Window | WindowStaysOnTopHint` leaves resize borders but no title
  bar for dragging. The flags are owned by `WindowPresentation`, with no QML
  override. See [Qt's Windows flag handling](https://github.com/qt/qtbase/blob/v6.11.2/src/plugins/platforms/windows/qwindowswindow.cpp#L592-L614).
- Each opening from hidden/minimized state uses the screen under the pointer,
  falling back to the window's screen and then the primary screen. The outer
  window, including native decorations, is centered at 90% of both dimensions
  of `QScreen::availableGeometry()` (excluding Dock/menu bar/taskbar).
- Coordinates remain in Qt logical pixels, including mixed-DPI and monitors
  positioned to the left of or above the primary screen.
- Raising an already visible window preserves its current size and position.
  Closing still hides it; priority enforcement stops while hidden/minimized.
- macOS uses `NSPopUpMenuWindowLevel + 1`, joins all Spaces and other apps' full
  screen/Stage Manager groups, and remains stationary in Mission Control.
  Native child dialogs/color pickers stay above the control window; remote
  rendering overlays use `CGWindowLevelForKey(kCGScreenSaverWindowLevelKey)`
  above both (currently scene 1000, dialogs 103, control 102). Both the control window
  and its dialogs must stay below `CGWindowLevelForKey(kCGDraggingWindowLevelKey)`.
  The earlier `NSScreenSaverWindowLevel` policy put them above that layer
  (1000/1001 versus 500), preventing native Finder drops from reaching the
  canvas despite the copy cursor. See the native window-level restriction
  documented by [Hammerspoon](https://www.hammerspoon.org/docs/hs.canvas.html#windowLevels).
- Windows uses `HWND_TOPMOST` without taking focus and pins the individual
  window with the shell's `IVirtualDesktopPinnedApps::PinView`. It does not pin
  every window of the application. Pinning is queried again after reopening or
  native handle recreation, and shell services are reacquired to recover after
  Explorer restarts.
- The coordinator applies scene priority on activation and after native
  surface/state changes, control-window reopening and popup activation. One
  500 ms timer covers all visible registered windows, without taking focus.
  Windows also constrains topmost raises in `WM_WINDOWPOSCHANGING` and listens
  to native show/reorder/foreground WinEvents (including native dialog loops).
  Scene windows have a stable front-to-back order; the control window stays
  below them without continuously raising and lowering itself.
- Scene surfaces retain transparency, `WindowTransparentForInput` and
  `WindowDoesNotAcceptFocus`. They use the complete screen geometry, including
  Dock/menu/taskbar areas. macOS scenes join all Spaces, remain stationary,
  use FullScreenAuxiliary and, on macOS 13+, CanJoinAllApplications for Stage
  Manager. Windows attempts to pin each visible scene window individually.
- Configuration is separate from native ordering: hidden PREPARE windows are
  never ordered in by AppKit. STOP unregisters surfaces before deferred
  destruction; queued enforcement only inspects currently registered windows.

Windows' pin interface is private, because the public
[IVirtualDesktopManager API](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ivirtualdesktopmanager)
only exposes desktop queries and moves. Its interface layout is isolated in
`WindowsWindowManager.cpp` (see
[VirtualDesktopAccessor](https://github.com/Ciantic/VirtualDesktopAccessor)). If
unavailable, the client logs a warning and attempts to move its window to the
foreground window's current desktop using the public API. This fallback can
lag by one timer interval and needs a foreground window with a desktop ID.

This is application window priority, not a security override: lock/login/UAC
secure desktops and exclusive fullscreen surfaces are controlled by the OS.
Another application may temporarily overtake the window before the next
enforcement tick. Windows' documented
[topmost contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)
guarantees placement above non-topmost windows, not an exclusive global rank.

## Display changes during a scene

`LocalScreenTopology` supplies the same ordered inventory to device discovery,
cursor mapping and remote rendering. On Windows, QScreen bindings are matched
by native HMONITOR, rather than assuming Qt's order matches EnumDisplayMonitors.
Window geometry stays in Qt logical pixels; advertised coordinates keep the
existing protocol conventions. Placeholder screens cannot satisfy preparation.

Each output retains its original source definition, screen binding and hardware
identity. macOS uses the CoreGraphics display UUID; Windows prefers a monitor
serial (manufacturer/model/serial), with a monitor device interface path fallback.
Geometry and DPI changes update the complete window rectangle and normalized
media destinations without replacing the scene or restarting automation.

Screen removal synchronously hides only that output before Qt can migrate it.
Its model, frame source, audio, playback and timers remain alive, including when
all outputs are absent. A returning screen resumes the current content only when
its identity matches exactly one available, unclaimed screen. Unknown or
ambiguous identities remain hidden; enumeration position and matching resolution
are never used to guess. A different/new screen receives no displaced content.

Before initial activation every target must exist. A screen lost before the
first-frame barrier completes remains pending until a genuine frame is presented;
an absent output cannot falsely acknowledge STARTED. After that barrier, removing
screens neither resets nor stops the run. Existing session, memory and shutdown
stop conditions still apply.

On the owner, screen discovery continues but `QuickCanvasHost` defers applying
new screen layouts to an active/preparing/stopping remote canvas. This preserves
the immutable screens and spans in periodic state snapshots. The latest pending
layout (including an empty one) is applied when presentation stops. Local test
playback keeps its existing screen-update behavior. No wire format changes.

## Validation

`tst_WindowPresentation` covers frame-inclusive geometry, negative screen
origins, QML binding, reopen/minimize restoration, preserving manual movement/resizing,
staying hidden, native demotion recovery and native handle recreation. macOS
also checks Space/fullscreen flags and scene-above-dialog-above-control ordering,
hidden native preparation and retired surfaces staying destroyed.
It checks that both the editor and its dialogs stay below the native drag
layer, including across enforcement ticks. `CanvasSelectionBackend` also
imports images and videos through the production QML `DropArea`, with the
production window policy enabled, using Qt events and a native Cocoa pasteboard.
Those injected events cannot establish WindowServer routing; a real Finder
drop remains part of native desktop acceptance.
On Windows it checks the native caption style, the enabled system Move command
and `WM_NCHITTEST` returning `HTCAPTION` over the title bar.

Before release, run on native macOS and Windows desktops: open on each monitor,
switch Spaces/virtual desktops, activate another app (including fullscreen),
open the color picker, hide/reopen, and change display scaling. On Windows also
restart Explorer and confirm Task View still shows the window on all desktops.
Native Windows desktop pinning cannot be validated by an offscreen test.

`RemoteSceneDisplayTopology` runs the renderer lifecycle executable with the
Qt offscreen/software plugins. It injects QPA screen-added, screen-removed and
geometry events for multiple screens, including negative origins, identity
ambiguity and unrelated replacement displays. It also covers snapshots during
absence, first-frame barriers and video playback while every output is absent.
`RemoteSceneLifecycle` verifies that owner-side topology updates do not mutate
an accepted scene and are applied after stop.

Desktop acceptance additionally includes unplug/replug during fades and video
playback, all outputs absent, resolution/DPI changes, transparent media over
Mouffette/dialogs/system bars, click-through and retained keyboard focus. Native
Windows behavior and real Finder event routing require their respective desktop
sessions; offscreen checks do not establish those guarantees.

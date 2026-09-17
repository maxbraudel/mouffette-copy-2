# Window presentation and remote scene priority

`WindowPresentation` owns the Qt Quick shell's opening geometry.
`WindowStackingCoordinator` owns the shared native stacking policy: active remote
scene surfaces stay above the control window and its dialogs/menus. Startup after bootstrap, tray activation and a second process's
activation request all use the same `open()` path. Tray clicks always open or
restore the window, bring it to the front and focus it, including when it is
already focused or on another desktop. They never hide it; use the native close
button to hide the window.

The Settings checkbox **App always on top** is enabled by default. Save applies
it immediately and persists `appAlwaysOnTop` in the profile settings; Cancel
leaves the current policy untouched. Existing settings without the key mean
`true`. Only the interactive shell and its owned dialogs use this setting.
Normal mode removes topmost priority and automatic all-desktop pinning while
still opening on the user's current desktop, including a macOS fullscreen Space.

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
  Closing still hides it; its priority is not enforced while hidden/minimized.
  Live scene surfaces keep their independent visibility and priority.
- macOS uses a Qt-owned, nonactivating `QNSPanel` in both priority modes. It can
  become key and accept input in another app's fullscreen Space without
  activating the application's previous desktop. Configure it before showing;
  never call Qt's Cocoa `raise()` here, which activates the whole process.
  The nonmodal `Qt::Dialog` type keeps that native panel with a standard title
  bar and standard-size close/minimize/fullscreen buttons. `Qt::Tool` adds the compact
  `NSWindowStyleMaskUtilityWindow` decoration in [Qt's Cocoa backend](https://github.com/qt/qtbase/blob/v6.11.2/src/plugins/platforms/cocoa/qcocoawindow.mm#L580-L581),
  shrinking these buttons. Use AppKit's normal metrics instead of manually
  resizing buttons or compensating in QML.
  `WindowFullscreenButtonHint` and `FullScreenPrimary` enable AppKit's native
  fullscreen button, including its icon, menu and action. No custom button
  handler is installed. Fullscreen uses a managed Space and normal window
  level; leaving fullscreen restores the current normal/topmost policy.
  Explicit opening also calls `orderFrontRegardless` after assigning keyboard
  focus: a nonactivating panel can become key while remaining behind another
  application's window. This raises it once without changing its normal/topmost
  level or activating an old Space (see [Apple's ordering contract](https://developer.apple.com/documentation/appkit/nswindow/orderfrontregardless%28%29)).
  Topmost mode uses `NSPopUpMenuWindowLevel + 1`, joins all Spaces and other apps'
  fullscreen/Stage Manager groups, and remains stationary in Mission Control.
  Normal mode uses `NSNormalWindowLevel` and `MoveToActiveSpace`; disabling
  topmost restores the original levels of owned native dialogs.
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
  Explorer restarts. Normal mode unpins the control window and uses
  `HWND_NOTOPMOST`. Before opening, the documented desktop manager moves it to
  the foreground desktop. An invisible, nonactivating native reference window
  supplies the current desktop ID when the foreground window has none.
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
  Manager. Hiding the macOS application does not hide a live scene (`canHide=NO`).
  Windows attempts to pin each visible scene window individually.
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

On the owner, `QuickCanvasHost` applies current screen layouts immediately,
including an empty layout, during preparation, playback and stopping. Removed
screens disappear without moving media or resetting the camera. The accepted
scene definition is stored independently: periodic playback snapshots use its
original screen definitions and normalized spans with current visual/video
state. Changing the displayed topology therefore does not redefine a live run.

`SystemMonitor` captures one native inventory for discovery and cursor mapping,
including physical dimensions and menu/Dock/taskbar zones. Windows uses
`EnumDisplayMonitors`; macOS uses CoreGraphics active displays and display modes,
so periodic reconciliation can detect a change even without a Qt notification.
An enumeration failure preserves the last inventory and suppresses ambiguous
cursor samples; a successfully captured empty inventory is authoritative.

The runtime captures on screen/volume events (150 ms topology debounce), every
second during an active incoming session, every five seconds while connected
without one, and at session opening/resumption/system wake. `WebSocketClient`
publishes changed discovery state first, then `remote_session_snapshot` to each
active owner. Sessions also receive an unchanged full snapshot every five
seconds. There is no wire format/version change. Under control-socket congestion
only the latest capture is retained; the relay likewise keeps one pending
snapshot per session, retried by the existing lease sweep. Cursor relay waits
until that topology has been enqueued. Identical received snapshots update
freshness without rebuilding screens or scheduling project writes.

## Validation

`tst_WindowPresentation` starts a separate Cocoa `tst_FullscreenHost` process to
verify that both priority modes open inside another application's fullscreen
Space, receive native keyboard events, and can switch priority without leaving that Space. The
helper exits after each test. It also verifies native inventory/Retina coordinate
parity, native minimization, normal-mode demotion and restoration of owned dialog levels,
including a dialog that already inherited its parent's elevated priority.
The helper's windowed mode reproduces another application covering the normal-mode
control window; repeated openings must restore native front-to-back ordering and
keyboard focus without hiding, moving, resizing or making the window topmost.
Fullscreen coverage also checks actual WindowServer ordering above the host.

The suite additionally covers frame-inclusive geometry, negative screen
origins, QML binding, reopen/minimize restoration, preserving manual movement/resizing,
staying hidden, native demotion recovery and native handle recreation. macOS
compares all three native button sizes and the title-bar height with a standard
window, including after priority changes and native handle recreation. It
also checks Space/fullscreen flags and scene-above-dialog-above-control ordering,
hidden native preparation and retired surfaces staying destroyed.
The green-button regression clicks the actual native button in both priority
modes, waits for AppKit's fullscreen entry/exit notifications, verifies screen
size and Qt state, changes priority in fullscreen, and checks geometry and
window policy restoration after exit.
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
`RemoteSceneLifecycle` verifies immediate owner-side screen replacement during
playback while the transmitted scene screens and spans stay unchanged.
`SystemMonitor` tests missed events, failure versus an empty inventory and
cursor sampling without re-enumeration. Connection/protocol suites cover active
snapshot publication, periodic freshness, coalescing, ordering and isolation.

Desktop acceptance additionally includes unplug/replug during fades and video
playback, all outputs absent, resolution/DPI changes, transparent media over
Mouffette/dialogs/system bars, click-through and retained keyboard focus. Native
Windows behavior and real Finder event routing require their respective desktop
sessions; offscreen checks do not establish those guarantees.

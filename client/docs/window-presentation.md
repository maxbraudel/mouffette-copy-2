# Main window presentation

`WindowPresentation` owns the Qt Quick shell's opening geometry and native
stacking policy. Startup after bootstrap, tray activation and a second process's
activation request all use the same `open()` path.

- Each opening from hidden/minimized state uses the screen under the pointer,
  falling back to the window's screen and then the primary screen. The outer
  window, including native decorations, is centered at 90% of both dimensions
  of `QScreen::availableGeometry()` (excluding Dock/menu bar/taskbar).
- Coordinates remain in Qt logical pixels, including mixed-DPI and monitors
  positioned to the left of or above the primary screen.
- Raising an already visible window preserves its current size and position.
  Closing still hides it; priority enforcement stops while hidden/minimized.
- macOS uses `NSScreenSaverWindowLevel`, joins all Spaces and other apps' full
  screen/Stage Manager groups, and remains stationary in Mission Control.
  Native child dialogs/color pickers stay above the control window; remote
  rendering overlays retain their own lower level.
- Windows uses `HWND_TOPMOST` without taking focus and pins the individual
  window with the shell's `IVirtualDesktopPinnedApps::PinView`. It does not pin
  every window of the application. Pinning is queried again after reopening or
  native handle recreation, and shell services are reacquired to recover after
  Explorer restarts.
- Priority is reapplied after native surface/state changes and every 500 ms
  while visible, without resizing or activating the window.

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

## Validation

`tst_WindowPresentation` covers frame-inclusive geometry, negative screen
origins, QML binding, reopen/minimize restoration, preserving manual resizing,
staying hidden, native demotion recovery and native handle recreation. macOS
also checks Space/fullscreen flags and dialog/remote-overlay ordering.

Before release, run on native macOS and Windows desktops: open on each monitor,
switch Spaces/virtual desktops, activate another app (including fullscreen),
open the color picker, hide/reopen, and change display scaling. On Windows also
restart Explorer and confirm Task View still shows the window on all desktops.
Native Windows desktop pinning cannot be validated by an offscreen test.

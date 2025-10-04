#ifndef MACWINDOWMANAGER_H
#define MACWINDOWMANAGER_H

#include <QtWidgets/QWidget>

class MacWindowManager {
public:
    static void setWindowAlwaysOnTop(QWidget* widget);
    // Configure a QWidget's native NSWindow to behave as a non-activating, click-through overlay:
    // - Visible across all Spaces (CanJoinAllSpaces)
    // - Auxiliary in full-screen (FullScreenAuxiliary)
    // - Hidden from Mission Control/App Exposé (Transient) and window cycling (IgnoresCycle)
    // - Transparent, no shadow, at status window level
    // - Optionally click-through (ignores mouse)
    static void setWindowAsGlobalOverlay(QWidget* widget, bool clickThrough = true);
};

#endif // MACWINDOWMANAGER_H
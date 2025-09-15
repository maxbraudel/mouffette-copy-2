#pragma once

#include <QColor>

// Global theme variables for overlay styling.
// Modify these at runtime to adjust the app's overlay look.
extern QColor gOverlayBackgroundColor;      // Default: semi-opaque dark used by media overlays
extern QColor gOverlayActiveBackgroundColor; // Default: accent used when active/selected
extern int    gOverlayCornerRadiusPx;        // Default: 8px corner radius

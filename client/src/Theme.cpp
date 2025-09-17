#include "Theme.h"

QColor gOverlayBackgroundColor = QColor(50, 50, 50, 240);        // default semi-opaque dark
// Use a softer active tint matching the previous loop button look (50% blend of base and accent)
// Base: (50,50,50), Accent: (74,144,226) => Blend(0.5) ≈ (62,97,138)
QColor gOverlayActiveBackgroundColor = QColor(62, 97, 138, 240);
int    gOverlayCornerRadiusPx = 8;

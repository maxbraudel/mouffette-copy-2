#include "AppColors.h"
#include <QApplication>
#include <QPalette>

namespace AppColors {

// ============================================================================
// CORE SYSTEM COLORS - CONFIGURABLE SOURCES
// ============================================================================

// Configuration: Change these to customize color behavior
ColorSource gAppBorderColorSource = ColorSource(QPalette::Text, 50);        // Dynamic: Mid palette - more visible borders
ColorSource gInteractionBackgroundColorSource = ColorSource(QPalette::Text, 8); // Dynamic: Text palette
ColorSource gWindowBackgroundColorSource = ColorSource(QPalette::Base); // Dynamic: Base palette

// ============================================================================
// BRAND COLORS
// ============================================================================

QColor gBrandBlue = QColor(74, 144, 226);              // #4a90e2
QColor gBrandBlueLight = QColor(74, 144, 226, 38);     // rgba(74,144,226,0.15)
QColor gBrandBlueDark = QColor(31, 78, 168);           // #1f4ea8

// ============================================================================
// STATUS COLORS
// ============================================================================

QColor gStatusConnectedText = QColor(76, 155, 80);     // Green text #2E7D32
QColor gStatusConnectedBg = QColor(76, 175, 80, 38);   // Green background with transparency
QColor gStatusWarningText = QColor(255, 160, 0);       // Orange text #FB8C00
QColor gStatusWarningBg = QColor(255, 152, 0, 38);     // Orange background with transparency
QColor gStatusErrorText = QColor(255, 87, 83);         // Red text #E53935
QColor gStatusErrorBg = QColor(244, 67, 54, 38);       // Red background with transparency

// ============================================================================
// BUTTON COLORS
// ============================================================================

QColor gButtonNormalBg = QColor(128, 128, 128, 20);     // Normal button (0.08 * 255 ≈ 20)
QColor gButtonHoverBg = QColor(128, 128, 128, 41);      // Button hover (0.16 * 255 ≈ 41)
QColor gButtonPressedBg = QColor(128, 128, 128, 61);    // Button pressed (0.24 * 255 ≈ 61)
QColor gButtonDisabledBg = QColor(128, 128, 128, 15);   // Button disabled (0.06 * 255 ≈ 15)

QColor gButtonPrimaryBg = QColor(74, 144, 226, 38);     // Primary button (0.15 * 255 ≈ 38)
QColor gButtonPrimaryHover = QColor(74, 144, 226, 56);  // Primary hover (0.22 * 255 ≈ 56)
QColor gButtonPrimaryPressed = QColor(74, 144, 226, 77); // Primary pressed (0.30 * 255 ≈ 77)
QColor gButtonPrimaryDisabled = QColor(74, 144, 226, 26); // Primary disabled (0.10 * 255 ≈ 26)

// Launch Remote Scene button colors (magenta theme)
QColor gLaunchRemoteSceneText = QColor(255, 150, 255);     // Magenta text
QColor gLaunchRemoteSceneBg = QColor(255, 0, 255, 38);   // Magenta background (0.15 * 255 ≈ 38)
QColor gLaunchRemoteSceneHover = QColor(255, 0, 255, 56); // Magenta hover (0.22 * 255 ≈ 56)
QColor gLaunchRemoteScenePressed = QColor(255, 0, 255, 77); // Magenta pressed (0.30 * 255 ≈ 77)

// Launch Remote Scene loading state colors (blue theme like upload)
QColor gLaunchRemoteSceneLoadingText = QColor(74, 144, 226); // Blue text
QColor gLaunchRemoteSceneLoadingBg = QColor(74, 144, 226, 38); // Blue background (0.15 * 255 ≈ 38)

// Launch Test Scene button colors (magenta theme)
QColor gLaunchTestSceneText = QColor(255, 150, 255);     // Magenta text
QColor gLaunchTestSceneBg = QColor(255, 0, 255, 38);   // Magenta background (0.15 * 255 ≈ 38)
QColor gLaunchTestSceneHover = QColor(255, 0, 255, 56); // Magenta hover (0.22 * 255 ≈ 56)
QColor gLaunchTestScenePressed = QColor(255, 0, 255, 77); // Magenta pressed (0.30 * 255 ≈ 77)

// ============================================================================
// OVERLAY COLORS
// ============================================================================

QColor gOverlayBackgroundColor = QColor(50, 50, 50, 240);        // Semi-transparent dark
QColor gOverlayActiveBackgroundColor = QColor(52, 87, 128, 240); // Active overlay
QColor gOverlayTextColor = QColor(255, 255, 255, 230);      // White text (0.9 * 255 ≈ 230)
QColor gOverlayBorderColor = QColor(255, 255, 255, 50); // White border (0.2 * 255 ≈ 51)

// ============================================================================
// MEDIA COLORS
// ============================================================================

QColor gMediaProgressFill = QColor(45, 140, 255);      // Blue progress fill #2D8CFF
QColor gMediaProgressBg = QColor(255, 255, 255, 38);   // White progress background (0.15 * 255 ≈ 38)
QColor gMediaUploadedColor = QColor(46, 204, 113);     // Green for uploaded #2ecc71
QColor gMediaNotUploadedColor = QColor(243, 156, 18);  // Orange for not uploaded #f39c12

// ============================================================================
// SPECIAL BUTTON COLORS (for specific use cases)
// ============================================================================

QColor gButtonGreyBg = QColor(102, 102, 102);         // Grey button background #666
QColor gButtonGreyPressed = QColor(68, 68, 68);        // Grey button pressed #444
QColor gButtonBlueBg = QColor(45, 108, 223);           // Blue button background #2d6cdf
QColor gButtonBluePressed = QColor(31, 78, 168);       // Blue button pressed #1f4ea8
QColor gButtonGreenBg = QColor(22, 163, 74);           // Green button background #16a34a
QColor gButtonGreenPressed = QColor(21, 128, 61);      // Green button pressed #15803d

// ============================================================================
// MEDIA SETTINGS PANEL COLORS
// ============================================================================

QColor gMediaPanelActiveBg = QColor(74, 144, 226);     // Active media panel background
QColor gMediaPanelActiveBorder = QColor(74, 144, 226); // Active media panel border
QColor gMediaPanelInactiveBg = QColor(60, 60, 60);     // Inactive media panel background
QColor gMediaPanelInactiveBorder = QColor(200, 200, 200); // Inactive media panel border

// ============================================================================
// UTILITY COLORS
// ============================================================================

QColor gTextMuted = QColor(102, 102, 102);            // Muted gray text #666
QColor gTextSecondary = QColor(255, 255, 255, 217);    // Secondary white text (0.85 * 255 ≈ 217)
QColor gHoverHighlight = QColor(74, 144, 226, 28);     // Light blue hover

// ============================================================================
// SYSTEM UI ZONE COLORS
// ============================================================================
QColor gSystemTaskbarColor = QColor(0, 0, 0, 80);      // Default opaque black for taskbars

// ============================================================================
// SNAP INDICATOR CONFIG
// ============================================================================
QColor gSnapIndicatorColor = QColor(209, 0, 136);     // Snap guide color
qreal  gSnapIndicatorLineThickness = 2.0;              // Stroke thickness (cosmetic pixels)
qreal  gSnapIndicatorDashGap = 10.0;                    // Gap (pixels) between dash segments

// ============================================================================
// INITIALIZATION
// ============================================================================

QColor getCurrentColor(const ColorSource& source) {
    if (!qApp) {
        // Fallback when no QApplication available
        return source.type == ColorSource::Static ? source.staticColor : QColor(128, 128, 128);
    }
    
    QColor color;
    if (source.type == ColorSource::Static) {
        color = source.staticColor;
    } else {
        color = qApp->palette().color(source.group, source.role);
    }
    
    // Apply alpha override if specified
    if (source.alpha >= 0 && source.alpha <= 255) {
        color.setAlpha(source.alpha);
    }
    
    return color;
}

QString colorToCss(const QColor& color) {
    return QString("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString colorSourceToCss(const ColorSource& source) {
    return colorToCss(getCurrentColor(source));
}

} // namespace AppColors

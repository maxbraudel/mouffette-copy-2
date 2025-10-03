#ifndef APPCOLORS_H
#define APPCOLORS_H

#include <QColor>
#include <QString>
#include <QPalette>

/**
 * @brief Centralized color management for Mouffette Client
 * 
 * This file contains all color variables used throughout the application.
 * Edit these values to customize the application's color scheme.
 */

namespace AppColors {

// ============================================================================
// DYNAMIC PALETTE SYSTEM
// ============================================================================

/**
 * @brief Color source configuration - defines how a color should be obtained
 */
struct ColorSource {
    enum Type {
        Static,     // Fixed color value
        Palette     // Dynamic color from system palette
    };
    
    Type type;
    QColor staticColor;           // Used when type == Static
    QPalette::ColorRole role;     // Used when type == Palette
    QPalette::ColorGroup group;   // Used when type == Palette (default: Active)
    int alpha;                    // Alpha override (0-255, -1 = use original)
    
    // Constructors
    ColorSource(const QColor& color) 
        : type(Static), staticColor(color), role(QPalette::Base), group(QPalette::Active), alpha(-1) {}
    
    ColorSource(QPalette::ColorRole paletteRole, int alphaOverride = -1, QPalette::ColorGroup grp = QPalette::Active)
        : type(Palette), role(paletteRole), group(grp), alpha(alphaOverride) {}
};

// ============================================================================
// CORE SYSTEM COLORS - CONFIGURABLE SOURCES
// ============================================================================

extern ColorSource gAppBorderColorSource;               // Standard border color for all UI elements
extern ColorSource gInteractionBackgroundColorSource;   // Background color for main interaction areas (canvas, client list)
extern ColorSource gWindowBackgroundColorSource;        // Main window background color

// ============================================================================
// BRAND COLORS
// ============================================================================

extern QColor gBrandBlue;                    // Primary brand blue #4a90e2
extern QColor gBrandBlueLight;               // Light brand blue for backgrounds
extern QColor gBrandBlueDark;                // Dark brand blue for pressed states

// ============================================================================
// STATUS COLORS
// ============================================================================

extern QColor gStatusConnectedText;          // Green text for connected status
extern QColor gStatusConnectedBg;            // Green background for connected status
extern QColor gStatusWarningText;            // Orange text for warning status  
extern QColor gStatusWarningBg;              // Orange background for warning status
extern QColor gStatusErrorText;              // Red text for error status
extern QColor gStatusErrorBg;                // Red background for error status

// ============================================================================
// BUTTON COLORS
// ============================================================================

extern QColor gButtonNormalBg;               // Normal button background
extern QColor gButtonHoverBg;                // Button hover background
extern QColor gButtonPressedBg;              // Button pressed background
extern QColor gButtonDisabledBg;             // Button disabled background

extern QColor gButtonPrimaryBg;
extern QColor gButtonPrimaryHover;
extern QColor gButtonPrimaryPressed;
extern QColor gButtonPrimaryDisabled;

// Launch Remote Scene button colors
extern QColor gLaunchRemoteSceneText;
extern QColor gLaunchRemoteSceneBg;
extern QColor gLaunchRemoteSceneHover;
extern QColor gLaunchRemoteScenePressed;

// Launch Test Scene button colors
extern QColor gLaunchTestSceneText;
extern QColor gLaunchTestSceneBg;
extern QColor gLaunchTestSceneHover;
extern QColor gLaunchTestScenePressed;

// ============================================================================
// OVERLAY COLORS
// ============================================================================

extern QColor gOverlayBackgroundColor;       // Semi-transparent overlay background
extern QColor gOverlayActiveBackgroundColor; // Active overlay background
extern QColor gOverlayTextColor;             // Overlay text color
extern QColor gOverlayBorderColor;           // Overlay border color

// ============================================================================
// MEDIA COLORS
// ============================================================================

extern QColor gMediaProgressFill;            // Progress bar fill color
extern QColor gMediaProgressBg;              // Progress bar background
extern QColor gMediaUploadedColor;           // Uploaded status color
extern QColor gMediaNotUploadedColor;        // Not uploaded status color

// ============================================================================
// SPECIAL BUTTON COLORS
// ============================================================================

extern QColor gButtonGreyBg;                 // Grey button background
extern QColor gButtonGreyPressed;            // Grey button pressed
extern QColor gButtonBlueBg;                 // Blue button background
extern QColor gButtonBluePressed;            // Blue button pressed
extern QColor gButtonGreenBg;                // Green button background
extern QColor gButtonGreenPressed;           // Green button pressed

// ============================================================================
// MEDIA PANEL COLORS
// ============================================================================

extern QColor gMediaPanelActiveBg;           // Active media panel background
extern QColor gMediaPanelActiveBorder;       // Active media panel border
extern QColor gMediaPanelInactiveBg;         // Inactive media panel background
extern QColor gMediaPanelInactiveBorder;     // Inactive media panel border

// ============================================================================
// UTILITY COLORS
// ============================================================================

extern QColor gTextMuted;                    // Muted text color
extern QColor gTextSecondary;                // Secondary text color
extern QColor gHoverHighlight;               // Light blue hover highlight

// ============================================================================
// SYSTEM UI ZONE COLORS
// ============================================================================
extern QColor gSystemTaskbarColor;            // Color used to render remote taskbar uiZones

// ============================================================================
// SNAP INDICATOR CONFIG
// ============================================================================
extern QColor gSnapIndicatorColor;            // Color for snapping guide lines
extern qreal  gSnapIndicatorLineThickness;    // Stroke thickness (cosmetic px)
extern qreal  gSnapIndicatorDashGap;          // Gap between dashes (cosmetic px)

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Get current color from ColorSource (resolves palette references dynamically)
 */
QColor getCurrentColor(const ColorSource& source);

/**
 * @brief Convert QColor to CSS string format
 * 
 * Converts a QColor to rgba() CSS string format for use in stylesheets.
 * 
 * @param color The QColor to convert
 * @return QString CSS rgba() string
 */
QString colorToCss(const QColor& color);

/**
 * @brief Get CSS string for a ColorSource (resolves dynamically)
 */
QString colorSourceToCss(const ColorSource& source);

} // namespace AppColors

#endif // APPCOLORS_H

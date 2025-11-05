#include "frontend/ui/theme/AppColors.h"
#include <QApplication>
#include <QPalette>

namespace {

QFont::Weight cssWeightToQt(int cssWeight) {
    if (cssWeight >= 900) return QFont::Black;
    if (cssWeight >= 800) return QFont::ExtraBold;
    if (cssWeight >= 700) return QFont::Bold;
    if (cssWeight >= 600) return QFont::DemiBold;
    if (cssWeight >= 500) return QFont::Medium;
    if (cssWeight >= 400) return QFont::Normal;
    if (cssWeight >= 300) return QFont::Light;
    if (cssWeight >= 200) return QFont::ExtraLight;
    return QFont::Thin;
}

bool cssWeightIsBold(int cssWeight) {
    return cssWeight >= 600;
}

QString fontCssString(int weight, int sizePx) {
    return QString("font-weight: %1; font-size: %2px;")
        .arg(weight)
        .arg(sizePx);
}

} // namespace

namespace AppColors {

// ============================================================================
// CORE SYSTEM COLORS - CONFIGURABLE SOURCES
// ============================================================================

// Configuration: Change these to customize color behavior
ColorSource gAppBorderColorSource = ColorSource(QPalette::Text, QPalette::Base, 0.2f);  // Blend: 20% Text + 80% Base
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

int gCanvasButtonFontSizePx = 14;                       // Default canvas button font size
int gCanvasButtonFontWeight = 700;                      // Default canvas button font weight (bold)

int gCanvasMediaSettingsOptionsFontSizePx = 14;         // Default options font size
int gCanvasMediaSettingsOptionsFontWeightPx = 500;      // Default options font weight (medium)

int gCanvasMediaSettingsSectionHeadersFontSizePx = 14;  // Default section header font size
int gCanvasMediaSettingsSectionHeadersFontWeightPx = 800; // Default section header font weight (semi-bold)

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
QColor gOverlayActiveSliderFillColor = QColor(52, 87, 128, 255); // Brighter slider fill
QColor gOverlayTextColor = QColor(255, 255, 255, 230);      // White text (0.9 * 255 ≈ 230)
QColor gOverlayBorderColor = QColor(100, 100, 100, 255); // White border (0.2 * 255 ≈ 51)

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
    } else if (source.type == ColorSource::Palette) {
        color = qApp->palette().color(source.group, source.role);
    } else if (source.type == ColorSource::Blend) {
        // Blend two palette colors
        QColor color1 = qApp->palette().color(source.group, source.blendRole1);
        QColor color2 = qApp->palette().color(source.group, source.blendRole2);
        
        float ratio = qBound(0.0f, source.blendRatio, 1.0f);
        float inverseRatio = 1.0f - ratio;
        
        color.setRed(static_cast<int>(color1.red() * ratio + color2.red() * inverseRatio));
        color.setGreen(static_cast<int>(color1.green() * ratio + color2.green() * inverseRatio));
        color.setBlue(static_cast<int>(color1.blue() * ratio + color2.blue() * inverseRatio));
        color.setAlpha(static_cast<int>(color1.alpha() * ratio + color2.alpha() * inverseRatio));
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

QFont::Weight canvasButtonQtWeight() {
    return cssWeightToQt(gCanvasButtonFontWeight);
}

bool canvasButtonFontIsBold() {
    return cssWeightIsBold(gCanvasButtonFontWeight);
}

QString canvasButtonFontCss() {
    return fontCssString(gCanvasButtonFontWeight, gCanvasButtonFontSizePx);
}

void applyCanvasButtonFont(QFont& font) {
    font.setBold(canvasButtonFontIsBold());
    font.setWeight(canvasButtonQtWeight());
    if (gCanvasButtonFontSizePx > 0) {
        font.setPixelSize(gCanvasButtonFontSizePx);
    }
}

QFont::Weight canvasMediaSettingsOptionsQtWeight() {
    return cssWeightToQt(gCanvasMediaSettingsOptionsFontWeightPx);
}

bool canvasMediaSettingsOptionsFontIsBold() {
    return cssWeightIsBold(gCanvasMediaSettingsOptionsFontWeightPx);
}

QString canvasMediaSettingsOptionsFontCss() {
    return fontCssString(gCanvasMediaSettingsOptionsFontWeightPx, gCanvasMediaSettingsOptionsFontSizePx);
}

void applyCanvasMediaSettingsOptionsFont(QFont& font) {
    font.setBold(canvasMediaSettingsOptionsFontIsBold());
    font.setWeight(canvasMediaSettingsOptionsQtWeight());
    if (gCanvasMediaSettingsOptionsFontSizePx > 0) {
        font.setPixelSize(gCanvasMediaSettingsOptionsFontSizePx);
    }
}

QFont::Weight canvasMediaSettingsSectionHeadersQtWeight() {
    return cssWeightToQt(gCanvasMediaSettingsSectionHeadersFontWeightPx);
}

bool canvasMediaSettingsSectionHeadersFontIsBold() {
    return cssWeightIsBold(gCanvasMediaSettingsSectionHeadersFontWeightPx);
}

QString canvasMediaSettingsSectionHeadersFontCss() {
    return fontCssString(gCanvasMediaSettingsSectionHeadersFontWeightPx, gCanvasMediaSettingsSectionHeadersFontSizePx);
}

void applyCanvasMediaSettingsSectionHeadersFont(QFont& font) {
    font.setBold(canvasMediaSettingsSectionHeadersFontIsBold());
    font.setWeight(canvasMediaSettingsSectionHeadersQtWeight());
    if (gCanvasMediaSettingsSectionHeadersFontSizePx > 0) {
        font.setPixelSize(gCanvasMediaSettingsSectionHeadersFontSizePx);
    }
}

} // namespace AppColors

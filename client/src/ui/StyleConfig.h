#ifndef STYLECONFIG_H
#define STYLECONFIG_H

/**
 * @brief Global style configuration variables
 * 
 * These variables control the visual appearance of UI elements throughout
 * the application. They are defined in MainWindow.cpp and accessed by
 * various UI components.
 * 
 * TODO: Move to ThemeManager singleton in Phase 2 of refactoring
 */

// Global window content margins (between all content and window borders)
extern int gWindowContentMarginTop;
extern int gWindowContentMarginRight;
extern int gWindowContentMarginBottom;
extern int gWindowContentMarginLeft;

// Global window appearance variables
extern int gWindowBorderRadiusPx;

// Global inner content gap between sections
extern int gInnerContentGap;

// Global dynamicBox configuration for standardized buttons and status indicators
extern int gDynamicBoxMinWidth;
extern int gDynamicBoxHeight;
extern int gDynamicBoxBorderRadius;
extern int gDynamicBoxFontPx;

// Remote client info container configuration
extern int gRemoteClientContainerPadding;

// Global title text configuration
extern int gTitleTextFontSize;
extern int gTitleTextHeight;

#endif // STYLECONFIG_H

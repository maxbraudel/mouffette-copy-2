#ifndef STYLECONFIG_H
#define STYLECONFIG_H

#include "ui/theme/ThemeManager.h"

/**
 * @brief Global style configuration variables
 * 
 * [Phase 17] These macros provide backward compatibility access to style
 * configuration now managed by ThemeManager singleton.
 * 
 * Previously these were global int variables defined in MainWindow.cpp.
 * They have been migrated to ThemeManager::StyleConfig for better encapsulation.
 */

// [Phase 17] Backward compatibility macros - delegate to ThemeManager
#define gWindowContentMarginTop (ThemeManager::instance()->getWindowContentMarginTop())
#define gWindowContentMarginRight (ThemeManager::instance()->getWindowContentMarginRight())
#define gWindowContentMarginBottom (ThemeManager::instance()->getWindowContentMarginBottom())
#define gWindowContentMarginLeft (ThemeManager::instance()->getWindowContentMarginLeft())
#define gWindowBorderRadiusPx (ThemeManager::instance()->getWindowBorderRadiusPx())
#define gInnerContentGap (ThemeManager::instance()->getInnerContentGap())
#define gDynamicBoxMinWidth (ThemeManager::instance()->getDynamicBoxMinWidth())
#define gDynamicBoxHeight (ThemeManager::instance()->getDynamicBoxHeight())
#define gDynamicBoxBorderRadius (ThemeManager::instance()->getDynamicBoxBorderRadius())
#define gDynamicBoxFontPx (ThemeManager::instance()->getDynamicBoxFontPx())
#define gRemoteClientContainerPadding (ThemeManager::instance()->getRemoteClientContainerPadding())
#define gTitleTextFontSize (ThemeManager::instance()->getTitleTextFontSize())
#define gTitleTextHeight (ThemeManager::instance()->getTitleTextHeight())

#endif // STYLECONFIG_H

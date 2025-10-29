#include "ThemeManager.h"
#include "../MainWindow.h"
#include "AppColors.h"
#include "OverlayPanels.h"
#include "ScreenCanvas.h"
#include "RemoteClientInfoManager.h"
#include "TopBarManager.h"
#include "../ui/pages/ClientListPage.h"
#include "../ui/pages/CanvasViewPage.h"
#include <climits>
#include <QLabel>
#include <QFrame>
#include <QPushButton>

ThemeManager* ThemeManager::s_instance = nullptr;

ThemeManager* ThemeManager::instance()
{
    if (!s_instance) {
        s_instance = new ThemeManager();
    }
    return s_instance;
}

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent),
      m_config()
{
    // Default configuration is set in StyleConfig struct
}

void ThemeManager::setStyleConfig(const StyleConfig& config)
{
    m_config = config;
    emit styleConfigChanged();
}

void ThemeManager::applyPillButton(QPushButton* button)
{
    if (!button) return;
    
    // Avoid platform default/autoDefault enlarging the control on macOS
    button->setAutoDefault(false);
    button->setDefault(false);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumWidth(m_config.dynamicBoxMinWidth);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    
    // Enforce exact visual height via stylesheet (min/max-height) and fixed padding
    button->setStyleSheet(QString(
        "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %4px; border: 1px solid %1;"
        " border-radius: %2px; background-color: %5; color: palette(buttonText);"
        " min-height: %3px; max-height: %3px; text-align: center; }"
        "QPushButton:hover { background-color: %6; }"
        "QPushButton:pressed { background-color: %7; }"
        "QPushButton:disabled { color: palette(mid); border-color: %1; background-color: %8; }"
    ).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource))
     .arg(m_config.dynamicBoxBorderRadius)
     .arg(m_config.dynamicBoxHeight)
     .arg(m_config.dynamicBoxFontPx)
     .arg(AppColors::colorToCss(AppColors::gButtonNormalBg))
     .arg(AppColors::colorToCss(AppColors::gButtonHoverBg))
     .arg(AppColors::colorToCss(AppColors::gButtonPressedBg))
     .arg(AppColors::colorToCss(AppColors::gButtonDisabledBg)));
    
    // Final enforcement (some styles ignore min/max rules)
    button->setFixedHeight(m_config.dynamicBoxHeight);
}

void ThemeManager::applyPrimaryButton(QPushButton* button)
{
    if (!button) return;
    
    // Avoid platform default/autoDefault enlarging the control on macOS
    button->setAutoDefault(false);
    button->setDefault(false);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumWidth(m_config.dynamicBoxMinWidth);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    
    button->setStyleSheet(QString(
        "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %4px; border: 1px solid %1;"
        " border-radius: %2px; background-color: %5; color: %9;"
        " min-height: %3px; max-height: %3px; }"
        "QPushButton:hover { background-color: %6; }"
        "QPushButton:pressed { background-color: %7; }"
        "QPushButton:disabled { color: palette(mid); border-color: %1; background-color: %8; }"
    ).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource))
     .arg(m_config.dynamicBoxBorderRadius)
     .arg(m_config.dynamicBoxHeight)
     .arg(m_config.dynamicBoxFontPx)
     .arg(AppColors::colorToCss(AppColors::gButtonPrimaryBg))
     .arg(AppColors::colorToCss(AppColors::gButtonPrimaryHover))
     .arg(AppColors::colorToCss(AppColors::gButtonPrimaryPressed))
     .arg(AppColors::colorToCss(AppColors::gButtonPrimaryDisabled))
     .arg(AppColors::gBrandBlue.name()));
    
    // Final enforcement (some styles ignore min/max rules)
    button->setFixedHeight(m_config.dynamicBoxHeight);
}

void ThemeManager::applyStatusBox(QLabel* label, const QString& borderColor, 
                                   const QString& bgColor, const QString& textColor)
{
    if (!label) return;
    
    label->setMinimumWidth(m_config.dynamicBoxMinWidth);
    label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QString(
        "QLabel { padding: 0px 8px; font-size: %5px; border: 1px solid %1; border-radius: %2px; "
        "background-color: %3; color: %4; font-weight: bold; min-height: %6px; max-height: %6px; }"
    ).arg(borderColor)
     .arg(m_config.dynamicBoxBorderRadius)
     .arg(bgColor)
     .arg(textColor)
     .arg(m_config.dynamicBoxFontPx)
     .arg(m_config.dynamicBoxHeight));
    
    // Final enforcement
    label->setFixedHeight(m_config.dynamicBoxHeight);
}

void ThemeManager::applyTitleText(QLabel* label)
{
    if (!label) return;
    
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    label->setStyleSheet(QString(
        "QLabel { font-size: %1px; font-weight: bold; color: palette(text); min-height: %2px; max-height: %2px; }"
    ).arg(m_config.titleTextFontSize)
     .arg(m_config.titleTextHeight));
    
    label->setFixedHeight(m_config.titleTextHeight);
}

void ThemeManager::applyListWidgetStyle(QListWidget* listWidget)
{
    if (!listWidget) return;
    
    listWidget->setStyleSheet(
        QString("QListWidget { "
        "   border: 1px solid %1; "
        "   border-radius: 5px; "
        "   padding: 0px; "
        "   background-color: %2; "
        "   outline: none; "
        "}"
        "QListWidget::item { "
        "   padding: 10px; "
        "}"
        "QListWidget::item:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "}"
        "QListWidget::item:selected { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:active { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:hover { "
        "   background-color: %3; "
        "   color: palette(text); "
        "}")
            .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource))
            .arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
            .arg(AppColors::colorToCss(AppColors::gHoverHighlight))
    );
}

int ThemeManager::getUploadButtonMaxWidth() const
{
    return (gMediaListOverlayAbsoluteMaxWidthPx > 0) ? gMediaListOverlayAbsoluteMaxWidthPx : INT_MAX;
}

void ThemeManager::updateAllWidgetStyles(MainWindow* mainWindow)
{
    if (!mainWindow) return;
    
    // Re-apply stylesheets that use ColorSource to pick up theme changes
    QWidget* centralWidget = mainWindow->centralWidget();
    if (centralWidget) {
        centralWidget->setStyleSheet(QString(
            "QWidget#CentralRoot { background-color: %1; }"
        ).arg(AppColors::colorSourceToCss(AppColors::gWindowBackgroundColorSource)));
    }
    
    // Note: Client list styling now handled by ClientListPage
    
    // Ensure the client list page title uses the same text color as other texts
    QLabel* pageTitleLabel = mainWindow->findChild<QLabel*>("PageTitleLabel");
    if (pageTitleLabel) {
        pageTitleLabel->setStyleSheet(QString(
            "QLabel { "
            "    background: transparent; "
            "    border: none; "
            "    font-size: %1px; "
            "    font-weight: bold; "
            "    color: palette(text); "
            "}").arg(m_config.titleTextFontSize)
        );
    }
    
    // Update canvas container via CanvasViewPage
    CanvasViewPage* canvasViewPage = mainWindow->findChild<CanvasViewPage*>();
    if (canvasViewPage) {
        QWidget* canvasContainer = canvasViewPage->getCanvasContainer();
        if (canvasContainer) {
            canvasContainer->setStyleSheet(
                QString("QWidget#CanvasContainer { "
                "   background-color: %2; "
                "   border: 1px solid %1; "
                "   border-radius: 5px; "
                "}").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource))
                    .arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
            );
        }
    }
    
    // Update remote client info container border and separators via manager
    RemoteClientInfoManager* remoteManager = mainWindow->findChild<RemoteClientInfoManager*>();
    QWidget* remoteContainer = remoteManager ? remoteManager->getContainer() : nullptr;
    if (remoteContainer) {
        const QString containerStyle = QString(
            "QWidget { "
            "    background-color: transparent; "
            "    color: palette(button-text); "
            "    border: 1px solid %3; "
            "    border-radius: %1px; "
            "    min-height: %2px; "
            "    max-height: %2px; "
            "}"
        ).arg(m_config.dynamicBoxBorderRadius)
         .arg(m_config.dynamicBoxHeight)
         .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource));
        remoteContainer->setStyleSheet(containerStyle);
        
        // Update separators in remote client info
        QList<QFrame*> separators = remoteContainer->findChildren<QFrame*>();
        for (QFrame* separator : separators) {
            if (separator && separator->frameShape() == QFrame::VLine) {
                separator->setStyleSheet(QString("QFrame { color: %1; }")
                    .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
            }
        }
    }
    
    // Update local client info container border via TopBarManager
    TopBarManager* topBarManager = mainWindow->findChild<TopBarManager*>();
    QWidget* localContainer = topBarManager ? topBarManager->getLocalClientInfoContainer() : nullptr;
    if (localContainer) {
        const QString containerStyle = QString(
            "QWidget { "
            "    background-color: transparent; "
            "    color: palette(button-text); "
            "    border: 1px solid %3; "
            "    border-radius: %1px; "
            "    min-height: %2px; "
            "}"
        ).arg(m_config.dynamicBoxBorderRadius)
         .arg(m_config.dynamicBoxHeight)
         .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource));
        localContainer->setStyleSheet(containerStyle);
        
        // Update separators in local client info
        QList<QFrame*> localSeparators = localContainer->findChildren<QFrame*>();
        for (QFrame* separator : localSeparators) {
            if (separator && separator->frameShape() == QFrame::VLine) {
                separator->setStyleSheet(QString("QFrame { color: %1; }")
                    .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
            }
        }
    }
    
    // Update all buttons that use gAppBorderColorSource
    QList<QPushButton*> buttons = mainWindow->findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button && button->styleSheet().contains("border:") && 
            !button->styleSheet().contains("border: none") &&
            !button->styleSheet().contains("background: transparent")) {
            // Re-apply button styles - check if it's a primary or normal button
            QString currentStyle = button->styleSheet();
            if (currentStyle.contains(AppColors::gBrandBlue.name())) {
                applyPrimaryButton(button);
            } else if (currentStyle.contains("QPushButton")) {
                applyPillButton(button);
            }
        }
    }
    // Note: Client list style updates now handled by ClientListPage
}


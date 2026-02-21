#include "frontend/managers/ui/UploadButtonStyleManager.h"
#include "MainWindow.h"
#include "backend/network/UploadManager.h"
#include "backend/domain/session/SessionManager.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "frontend/ui/theme/AppColors.h"
#include "frontend/ui/theme/ThemeManager.h"
#include "frontend/ui/theme/StyleConfig.h"

// Use the fully qualified name
using CanvasSession = SessionManager::CanvasSession;

UploadButtonStyleManager::UploadButtonStyleManager(MainWindow* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void UploadButtonStyleManager::applyUploadButtonStyle(QPushButton* uploadButton) {
    if (!uploadButton) return;
    
    UploadManager* uploadManager = m_mainWindow->getUploadManager();
    if (!uploadManager) return;
    
    // Recalculate stored default font so runtime changes to typography propagate
    QFont defaultFont = getDefaultButtonFont();
    const QString canvasFontCss = AppColors::canvasButtonFontCss();
    
    // If button is in overlay, use custom overlay styling
    if (m_mainWindow->getUploadButtonInOverlay()) {
        if (!m_mainWindow->isRemoteOverlayActionsEnabled()) {
            uploadButton->setEnabled(false);
            uploadButton->setCheckable(false);
            uploadButton->setChecked(false);
            uploadButton->setStyleSheet(generateOverlayDisabledStyle());
            uploadButton->setFont(defaultFont);
            uploadButton->setFixedHeight(40);
            uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
            return;
        }
        
        const QString target = uploadManager->targetClientId();
        SessionManager* sessionManager = m_mainWindow->getSessionManager();
        const CanvasSession* targetSession = sessionManager->findSession(target);
        const bool sessionHasRemote = targetSession && targetSession->upload.remoteFilesPresent;
        const bool managerHasActiveForTarget = uploadManager->hasActiveUpload() &&
                                               uploadManager->activeUploadTargetClientId() == target;
        const bool remoteActive = sessionHasRemote || managerHasActiveForTarget;
        
        bool remoteSceneLaunched = isRemoteSceneLaunchedForButton(uploadButton);

        if (uploadManager->isUploading()) {
            if (uploadManager->isCancelling()) {
                uploadButton->setText("Cancelling…");
                uploadButton->setEnabled(false);
                uploadButton->setFont(defaultFont);
            } else {
                if (uploadButton->text() == "Upload") {
                    uploadButton->setText("Preparing");
                }
                uploadButton->setEnabled(true);
                uploadButton->setFont(getMonospaceFont());
            }
            uploadButton->setStyleSheet(generateOverlayUploadingStyle());
        } else if (uploadManager->isFinalizing()) {
            uploadButton->setText("Finalizing…");
            uploadButton->setEnabled(false);
            uploadButton->setStyleSheet(generateOverlayUploadingStyle());
            uploadButton->setFont(defaultFont);
        } else if (remoteActive) {
            const bool hasUnuploaded = hasUnuploadedFilesForTarget(target);
            if (target.isEmpty() || hasUnuploaded) {
                uploadButton->setText("Upload");
                uploadButton->setEnabled(!remoteSceneLaunched);
                uploadButton->setStyleSheet(remoteSceneLaunched ? generateOverlayDisabledStyle() : generateOverlayIdleStyle());
                uploadButton->setFont(defaultFont);
            } else {
                uploadButton->setText("Unload");
                uploadButton->setEnabled(!remoteSceneLaunched);
                uploadButton->setStyleSheet(remoteSceneLaunched ? generateOverlayDisabledStyle() : generateOverlayUnloadStyle());
                uploadButton->setFont(defaultFont);
            }
        } else {
            uploadButton->setText("Upload");
            uploadButton->setEnabled(!remoteSceneLaunched);
            uploadButton->setStyleSheet(remoteSceneLaunched ? generateOverlayDisabledStyle() : generateOverlayIdleStyle());
            uploadButton->setFont(defaultFont);
        }
        uploadButton->setFixedHeight(40);
        uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
        return;
    }
    
    // Regular button (not in overlay)
    if (!m_mainWindow->isRemoteOverlayActionsEnabled()) {
        uploadButton->setEnabled(false);
        uploadButton->setCheckable(false);
        uploadButton->setChecked(false);
        return;
    }

    const QString target = uploadManager->targetClientId();
    SessionManager* sessionManager = m_mainWindow->getSessionManager();
    const CanvasSession* targetSession = sessionManager->findSession(target);
    const bool sessionHasRemote = targetSession && targetSession->upload.remoteFilesPresent;
    const bool managerHasActiveForTarget = uploadManager->hasActiveUpload() &&
                                           uploadManager->activeUploadTargetClientId() == target;
    const bool remoteActive = sessionHasRemote || managerHasActiveForTarget;

    if (uploadManager->isUploading()) {
        if (uploadManager->isCancelling()) {
            uploadButton->setText("Cancelling…");
            uploadButton->setEnabled(false);
        } else {
            if (uploadButton->text() == "Upload to Client") {
                uploadButton->setText("Preparing download");
            }
            uploadButton->setEnabled(true);
        }
        uploadButton->setCheckable(true);
        uploadButton->setChecked(true);
        uploadButton->setStyleSheet(generateRegularBlueStyle());
        uploadButton->setFixedHeight(gDynamicBoxHeight);
        uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
        uploadButton->setFont(getMonospaceFont());
    } else if (uploadManager->isFinalizing()) {
        uploadButton->setCheckable(true);
        uploadButton->setChecked(true);
        uploadButton->setEnabled(false);
        uploadButton->setText("Finalizing…");
        uploadButton->setStyleSheet(generateRegularBlueStyle());
        uploadButton->setFixedHeight(gDynamicBoxHeight);
        uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
        uploadButton->setFont(defaultFont);
    } else if (remoteActive) {
        if (target.isEmpty() || hasUnuploadedFilesForTarget(target)) {
            uploadButton->setCheckable(false);
            uploadButton->setChecked(false);
            uploadButton->setEnabled(true);
            uploadButton->setText("Upload to Client");
            uploadButton->setStyleSheet(generateRegularGreyStyle());
            uploadButton->setFixedHeight(gDynamicBoxHeight);
            uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
            uploadButton->setFont(defaultFont);
        } else {
            uploadButton->setCheckable(true);
            uploadButton->setChecked(true);
            uploadButton->setEnabled(true);
            uploadButton->setText("Remove all files");
            uploadButton->setStyleSheet(generateRegularGreenStyle());
            uploadButton->setFixedHeight(gDynamicBoxHeight);
            uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
            uploadButton->setFont(defaultFont);
        }
    } else {
        uploadButton->setCheckable(false);
        uploadButton->setChecked(false);
        uploadButton->setEnabled(true);
        uploadButton->setText("Upload to Client");
        uploadButton->setStyleSheet(generateRegularGreyStyle());
        uploadButton->setFixedHeight(gDynamicBoxHeight);
        uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
        uploadButton->setFont(defaultFont);
    }
}

void UploadButtonStyleManager::updateUploadButtonProgress(QPushButton* uploadButton, int percent, int filesCompleted, int totalFiles) {
    if (!uploadButton) return;
    
    UploadManager* uploadManager = m_mainWindow->getUploadManager();
    if (!uploadManager) return;
    
    if ((uploadManager->isUploading() || uploadManager->isFinalizing()) && !uploadManager->isCancelling()) {
        if (uploadManager->isFinalizing()) {
            uploadButton->setText("Finalizing…");
        } else {
            uploadButton->setText(QString("Uploading (%1/%2) %3%")
                                .arg(filesCompleted)
                                .arg(totalFiles)
                                .arg(percent));
        }
    }
    applyUploadButtonStyle(uploadButton);
}

// Style generation helpers
QString UploadButtonStyleManager::generateOverlayIdleStyle() const {
    const QString canvasFontCss = AppColors::canvasButtonFontCss();
    return QString(
        "QPushButton { "
        "    padding: 0px 20px; "
        "    %1 "
        "    color: %2; "
        "    background: transparent; "
        "    border: none; "
        "    border-radius: 0px; "
        "    text-align: center; "
        "} "
        "QPushButton:hover { "
        "    color: white; "
        "    background: rgba(255,255,255,0.05); "
        "} "
        "QPushButton:pressed { "
        "    color: white; "
        "    background: rgba(255,255,255,0.1); "
        "}"
    ).arg(canvasFontCss, AppColors::colorToCss(AppColors::gOverlayTextColor));
}

QString UploadButtonStyleManager::generateOverlayUploadingStyle() const {
    const QString canvasFontCss = AppColors::canvasButtonFontCss();
    return QString(
        "QPushButton { "
        "    padding: 0px 20px; "
        "    %1 "
        "    color: %2; "
        "    background: %3; "
        "    border: none; "
        "    border-radius: 0px; "
        "    text-align: center; "
        "} "
        "QPushButton:hover { "
        "    color: %2; "
        "    background: %4; "
        "} "
        "QPushButton:pressed { "
        "    color: %2; "
        "    background: %5; "
        "}"
    ).arg(canvasFontCss,
          AppColors::gBrandBlue.name(),
          AppColors::colorToCss(AppColors::gButtonPrimaryBg),
          AppColors::colorToCss(AppColors::gButtonPrimaryHover),
          AppColors::colorToCss(AppColors::gButtonPrimaryPressed));
}

QString UploadButtonStyleManager::generateOverlayUnloadStyle() const {
    const QString canvasFontCss = AppColors::canvasButtonFontCss();
    return QString(
        "QPushButton { "
        "    padding: 0px 20px; "
        "    %1 "
        "    color: %2; "
        "    background: %3; "
        "    border: none; "
        "    border-radius: 0px; "
        "    text-align: center; "
        "} "
        "QPushButton:hover { "
        "    color: %2; "
        "    background: rgba(76, 175, 80, 56); "
        "} "
        "QPushButton:pressed { "
        "    color: %2; "
        "    background: rgba(76, 175, 80, 77); "
        "}"
    ).arg(canvasFontCss,
          AppColors::colorToCss(AppColors::gMediaUploadedColor),
          AppColors::colorToCss(AppColors::gStatusConnectedBg));
}

QString UploadButtonStyleManager::generateOverlayDisabledStyle() const {
    return ScreenCanvas::overlayDisabledButtonStyle();
}

QString UploadButtonStyleManager::generateRegularGreyStyle() const {
    return QString(
        "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %3px; background-color: %4; color: white; border-radius: %1px; min-height: %2px; max-height: %2px; } "
        "QPushButton:checked { background-color: %5; }"
    ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx).arg(AppColors::colorToCss(AppColors::gButtonGreyBg)).arg(AppColors::colorToCss(AppColors::gButtonGreyPressed));
}

QString UploadButtonStyleManager::generateRegularBlueStyle() const {
    return QString(
        "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %3px; background-color: %4; color: white; border-radius: %1px; min-height: %2px; max-height: %2px; } "
        "QPushButton:checked { background-color: %5; }"
    ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx).arg(AppColors::colorToCss(AppColors::gButtonBlueBg)).arg(AppColors::colorToCss(AppColors::gButtonBluePressed));
}

QString UploadButtonStyleManager::generateRegularGreenStyle() const {
    return QString(
        "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %3px; background-color: %4; color: white; border-radius: %1px; min-height: %2px; max-height: %2px; } "
        "QPushButton:checked { background-color: %5; }"
    ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx).arg(AppColors::colorToCss(AppColors::gButtonGreenBg)).arg(AppColors::colorToCss(AppColors::gButtonGreenPressed));
}

// State detection helpers
bool UploadButtonStyleManager::isRemoteSceneLaunchedForButton(QPushButton* button) const {
    SessionManager* sessionManager = m_mainWindow->getSessionManager();
    if (!sessionManager) return false;
    
    for (CanvasSession* session : sessionManager->getAllSessions()) {
        if (session->uploadButton == button && session->canvas) {
            return session->canvas->isRemoteSceneLaunched();
        }
    }
    return false;
}

bool UploadButtonStyleManager::hasUnuploadedFilesForTarget(const QString& targetClientId) const {
    return m_mainWindow->hasUnuploadedFilesForTarget(targetClientId);
}

// Font helpers
QFont UploadButtonStyleManager::getMonospaceFont() const {
#ifdef Q_OS_MACOS
    QFont mono("Menlo");
#else
    QFont mono("Courier New");
#endif
    AppColors::applyCanvasButtonFont(mono);
    return mono;
}

QFont UploadButtonStyleManager::getDefaultButtonFont() const {
    QFont defaultFont = m_mainWindow->getUploadButtonDefaultFont();
    AppColors::applyCanvasButtonFont(defaultFont);
    return defaultFont;
}

/**
 * @file CanvasViewPage.cpp
 * @brief Implementation of the canvas view page
 */

#include "frontend/ui/pages/CanvasViewPage.h"
#include "frontend/ui/theme/ThemeManager.h"
#include "frontend/ui/theme/AppColors.h"
#include "frontend/ui/theme/StyleConfig.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "frontend/ui/widgets/SpinnerWidget.h"
#include "backend/domain/models/ClientInfo.h"
#include <QTimer>

CanvasViewPage::CanvasViewPage(QWidget* parent)
    : QWidget(parent),
      m_layout(nullptr),
      m_remoteClientInfoWrapper(nullptr),
      m_remoteClientInfoContainer(nullptr),
      m_clientNameLabel(nullptr),
      m_remoteConnectionStatusLabel(nullptr),
      m_volumeIndicator(nullptr),
      m_remoteInfoSep1(nullptr),
      m_remoteInfoSep2(nullptr),
      m_inlineSpinner(nullptr),
      m_canvasContainer(nullptr),
      m_canvasStack(nullptr),
      m_canvasHostStack(nullptr),
      m_loadingSpinner(nullptr),
      m_screenCanvas(nullptr),
      m_backButton(nullptr),
      m_uploadButton(nullptr),
      m_uploadButtonInOverlay(false),
      m_spinnerOpacity(nullptr),
      m_spinnerFade(nullptr),
      m_canvasOpacity(nullptr),
      m_canvasFade(nullptr),
      m_volumeOpacity(nullptr),
      m_volumeFade(nullptr),
      m_remoteClientConnected(false),
      m_remoteOverlayActionsEnabled(false),
      m_fadeDurationMs(200),
      m_loaderFadeDurationMs(150)
{
    setupUI();
}

void CanvasViewPage::applyTitleText(QLabel* label) {
    ThemeManager::instance()->applyTitleText(label);
}

void CanvasViewPage::setupUI() {
    m_layout = new QVBoxLayout(this);
    m_layout->setSpacing(gInnerContentGap);
    m_layout->setContentsMargins(0, 0, 0, 0);
    
    // Create labels for remote client info (will be used in top bar container)
    m_clientNameLabel = new QLabel();
    applyTitleText(m_clientNameLabel);
    m_clientNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Remote connection status (to the right of hostname)
    m_remoteConnectionStatusLabel = new QLabel("DISCONNECTED");
    // Initial styling - will be updated by setRemoteConnectionStatus()
    m_remoteConnectionStatusLabel->setStyleSheet(
        QString("QLabel { "
        "    color: #E53935; "
        "    background-color: rgba(244,67,54,0.15); "
        "    border: none; "
        "    border-radius: 0px; "
        "    padding: 0px %2px; "
        "    font-size: %1px; "
        "    font-weight: bold; "
        "}").arg(gDynamicBoxFontPx).arg(gRemoteClientContainerPadding)
    );
    m_remoteConnectionStatusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_remoteConnectionStatusLabel->setFixedWidth(120); // Fixed width for consistency
    m_remoteConnectionStatusLabel->setAlignment(Qt::AlignCenter); // Center the text

    m_volumeIndicator = new QLabel("🔈 --");
    m_volumeIndicator->setStyleSheet("QLabel { font-size: 16px; color: palette(text); font-weight: bold; }");
    m_volumeIndicator->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeIndicator->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Volume indicator opacity effect
    m_volumeOpacity = new QGraphicsOpacityEffect(m_volumeIndicator);
    m_volumeIndicator->setGraphicsEffect(m_volumeOpacity);
    m_volumeOpacity->setOpacity(0.0);

    // Initialize remote client info container in top bar permanently
    initializeRemoteClientInfoInTopBar();
    
    // Canvas container holds spinner and canvas with a stacked layout
    m_canvasContainer = new QWidget();
    m_canvasContainer->setObjectName("CanvasContainer");
    // Remove minimum height to allow flexible window resizing
    // Ensure stylesheet background/border is actually painted
    m_canvasContainer->setAttribute(Qt::WA_StyledBackground, true);
    // Match the dark background used by the client list container via palette(base)
    // Canvas container previously had a bordered panel look; remove to emulate design-tool feel
    m_canvasContainer->setStyleSheet(
        QString("QWidget#CanvasContainer { "
        "   background-color: %2; "
        "   border: 1px solid %1; "
        "   border-radius: 5px; "
        "}").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource))
           .arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
    );
    QVBoxLayout* containerLayout = new QVBoxLayout(m_canvasContainer);
    // Remove inner padding so content goes right to the border edge
    containerLayout->setContentsMargins(0,0,0,0);
    containerLayout->setSpacing(0);
    m_canvasStack = new QStackedWidget();
    // Match client list container: base background, no border on inner stack
    m_canvasStack->setStyleSheet(
        "QStackedWidget { "
        "   background-color: transparent; "
        "   border: none; "
        "}"
    );
    containerLayout->addWidget(m_canvasStack);
    
    // Spinner page
    m_loadingSpinner = new SpinnerWidget();
    // Initial appearance (easy to tweak):
    m_loadingSpinner->setRadius(22);        // circle radius in px
    m_loadingSpinner->setLineWidth(6);      // line width in px
    m_loadingSpinner->setColor(AppColors::gBrandBlue); // brand blue
    m_loadingSpinner->setMinimumSize(QSize(48, 48));
    // Spinner page widget wraps the spinner centered
    QWidget* spinnerPage = new QWidget();
    QVBoxLayout* spinnerLayout = new QVBoxLayout(spinnerPage);
    spinnerLayout->setContentsMargins(0,0,0,0);
    spinnerLayout->setSpacing(0);
    spinnerLayout->addStretch();
    spinnerLayout->addWidget(m_loadingSpinner, 0, Qt::AlignCenter);
    spinnerLayout->addStretch();
    // Spinner page opacity effect & animation (fade entire loader area)
    m_spinnerOpacity = new QGraphicsOpacityEffect(spinnerPage);
    spinnerPage->setGraphicsEffect(m_spinnerOpacity);
    m_spinnerOpacity->setOpacity(0.0);
    m_spinnerFade = new QPropertyAnimation(m_spinnerOpacity, "opacity", this);
    m_spinnerFade->setDuration(m_loaderFadeDurationMs);
    m_spinnerFade->setStartValue(0.0);
    m_spinnerFade->setEndValue(1.0);

    // Canvas page
    QWidget* canvasPage = new QWidget();
    QVBoxLayout* canvasLayout = new QVBoxLayout(canvasPage);
    canvasLayout->setContentsMargins(0,0,0,0);
    canvasLayout->setSpacing(0);

    m_canvasHostStack = new QStackedWidget();
    m_canvasHostStack->setObjectName("CanvasHostStack");
    m_canvasHostStack->setStyleSheet(
        "QStackedWidget { "
        "   background-color: transparent; "
        "   border: none; "
        "}"
    );
    canvasLayout->addWidget(m_canvasHostStack);

    QWidget* emptyCanvasPlaceholder = new QWidget();
    m_canvasHostStack->addWidget(emptyCanvasPlaceholder);
    m_canvasHostStack->setCurrentWidget(emptyCanvasPlaceholder);
    
    // Canvas/content opacity effect & animation (apply to the page, not the QGraphicsView viewport to avoid heavy repaints)
    m_canvasOpacity = new QGraphicsOpacityEffect(canvasPage);
    canvasPage->setGraphicsEffect(m_canvasOpacity);
    m_canvasOpacity->setOpacity(0.0);
    m_canvasFade = new QPropertyAnimation(m_canvasOpacity, "opacity", this);
    m_canvasFade->setDuration(m_fadeDurationMs);
    m_canvasFade->setStartValue(0.0);
    m_canvasFade->setEndValue(1.0);

    // Add pages and container to main layout
    m_canvasStack->addWidget(spinnerPage); // index 0: spinner
    m_canvasStack->addWidget(canvasPage);  // index 1: canvas
    m_canvasStack->setCurrentIndex(1);     // default to canvas page hidden (opacity 0) until data
    m_layout->addWidget(m_canvasContainer, 1);

    // Volume label opacity effect & animation
    m_volumeFade = new QPropertyAnimation(m_volumeOpacity, "opacity", this);
    m_volumeFade->setDuration(m_fadeDurationMs);
    m_volumeFade->setStartValue(0.0);
    m_volumeFade->setEndValue(1.0);
}

void CanvasViewPage::createRemoteClientInfoContainer() {
    if (m_remoteClientInfoContainer) return;

    m_remoteClientInfoContainer = new QWidget();
    m_remoteClientInfoContainer->setObjectName("RemoteClientInfoContainer");
    m_remoteClientInfoContainer->setAttribute(Qt::WA_StyledBackground, true);
    m_remoteClientInfoContainer->setStyleSheet(
        QString("QWidget#RemoteClientInfoContainer { "
        "    background-color: %2; "
        "    border: 1px solid %1; "
        "    border-radius: %3px; "
        "    padding: 0px; "
        "}")
        .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource))
        .arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
        .arg(gDynamicBoxBorderRadius)
    );
    m_remoteClientInfoContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_remoteClientInfoContainer->setMinimumHeight(gDynamicBoxHeight);
    m_remoteClientInfoContainer->setMaximumHeight(gDynamicBoxHeight);

    QHBoxLayout* containerLayout = new QHBoxLayout(m_remoteClientInfoContainer);
    containerLayout->setContentsMargins(gRemoteClientContainerPadding, 0, gRemoteClientContainerPadding, 0);
    containerLayout->setSpacing(gRemoteClientContainerPadding);

    // Add hostname label (ensure no individual styling)
    m_clientNameLabel->setStyleSheet(
        QString("QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    padding: 0px %1px; "
        "    font-size: %2px; "
        "    font-weight: bold; "
        "}").arg(gRemoteClientContainerPadding).arg(gTitleTextFontSize)
    );
    containerLayout->addWidget(m_clientNameLabel);

    // First separator between hostname and status
    m_remoteInfoSep1 = new QFrame();
    m_remoteInfoSep1->setFrameShape(QFrame::VLine);
    m_remoteInfoSep1->setFrameShadow(QFrame::Sunken);
    m_remoteInfoSep1->setStyleSheet(QString("QFrame { color: %1; }")
        .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    m_remoteInfoSep1->setFixedWidth(1);
    containerLayout->addWidget(m_remoteInfoSep1);

    // Add status label (ensure no individual styling conflicts with dynamic updates)
    m_remoteConnectionStatusLabel->setStyleSheet(
        QString("QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    padding: 0px %1px; "
        "    font-size: %2px; "
        "    font-weight: bold; "
        "}").arg(gRemoteClientContainerPadding).arg(gDynamicBoxFontPx)
    );
    containerLayout->addWidget(m_remoteConnectionStatusLabel);

    // Spacer to push volume indicator to the right
    containerLayout->addStretch();

    // Second separator before volume indicator
    m_remoteInfoSep2 = new QFrame();
    m_remoteInfoSep2->setFrameShape(QFrame::VLine);
    m_remoteInfoSep2->setFrameShadow(QFrame::Sunken);
    m_remoteInfoSep2->setStyleSheet(QString("QFrame { color: %1; }")
        .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    m_remoteInfoSep2->setFixedWidth(1);
    containerLayout->addWidget(m_remoteInfoSep2);
    
    // Add volume indicator (ensure no individual styling)
    m_volumeIndicator->setStyleSheet(
        QString("QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    padding: 0px %1px; "
        "    font-size: 16px; "
        "    font-weight: bold; "
        "}").arg(gRemoteClientContainerPadding)
    );
    containerLayout->addWidget(m_volumeIndicator);
}

void CanvasViewPage::initializeRemoteClientInfoInTopBar() {
    createRemoteClientInfoContainer();
    
    if (!m_remoteClientInfoContainer) return;

    // The container is created but not added to any layout yet
    // It will be managed by ResponsiveLayoutManager
    m_remoteClientInfoContainer->hide();
}

void CanvasViewPage::updateClientNameDisplay(const ClientInfo& client) {
    if (!m_clientNameLabel) return;

    QString name = client.getMachineName().trimmed();
    QString platform = client.getPlatform().trimmed();

    if (name.isEmpty()) {
        name = tr("Unknown Machine");
    }

    QString text = name;
    if (!platform.isEmpty()) {
        text = QStringLiteral("%1 (%2)").arg(name, platform);
    }

    m_clientNameLabel->setText(text);

    if (m_remoteClientInfoContainer) {
        m_remoteClientInfoContainer->setToolTip(text);
    }
}

void CanvasViewPage::updateVolumeIndicator(int volumePercent) {
    if (!m_volumeIndicator) return;
    
    if (volumePercent < 0) {
        m_volumeIndicator->setText("🔈 --");
        return;
    }
    
    QString icon;
    if (volumePercent == 0) icon = "🔇";
    else if (volumePercent < 34) icon = "🔈";
    else if (volumePercent < 67) icon = "🔉";
    else icon = "🔊";
    
    m_volumeIndicator->setText(QString("%1 %2%").arg(icon).arg(volumePercent));
    
    // Don't force-show here; presence in layout is managed elsewhere
    if (m_volumeOpacity) {
        if (m_volumeOpacity->opacity() < 1.0) {
            m_volumeOpacity->setOpacity(1.0);
        }
    }
}

void CanvasViewPage::setRemoteConnectionStatus(const QString& status, bool propagateLoss) {
    if (!m_remoteConnectionStatusLabel) return;
    
    const QString up = status.toUpper();
    m_remoteConnectionStatusLabel->setText(up);
    
    if (up == "CONNECTED") {
        m_remoteClientConnected = true;
    } else if (up == "DISCONNECTED") {
        m_remoteClientConnected = false;
    } else if (up.startsWith("CONNECTING") || up == "ERROR") {
        m_remoteClientConnected = false;
    }

    // Apply same styling as main connection status with colored background
    QString textColor, bgColor;
    if (up == "CONNECTED") {
        textColor = AppColors::colorToCss(AppColors::gStatusConnectedText);
        bgColor = AppColors::colorToCss(AppColors::gStatusConnectedBg);
        if (m_inlineSpinner) {
            // Stop spinner if it exists
            m_inlineSpinner->hide();
        }
    } else if (up == "ERROR" || up.startsWith("CONNECTING") || up.startsWith("RECONNECTING")) {
        textColor = AppColors::colorToCss(AppColors::gStatusWarningText);
        bgColor = AppColors::colorToCss(AppColors::gStatusWarningBg);
    } else {
        textColor = AppColors::colorToCss(AppColors::gStatusErrorText);
        bgColor = AppColors::colorToCss(AppColors::gStatusErrorBg);
    }
    
    m_remoteConnectionStatusLabel->setStyleSheet(
        QString("QLabel { "
        "    color: %1; "
        "    background-color: %2; "
        "    border: none; "
        "    border-radius: 0px; "
        "    padding: 0px %4px; "
        "    font-size: %3px; "
        "    font-weight: bold; "
        "}").arg(textColor).arg(bgColor).arg(gDynamicBoxFontPx).arg(gRemoteClientContainerPadding)
    );

    refreshOverlayActionsState(up == "CONNECTED", propagateLoss);
}

void CanvasViewPage::refreshOverlayActionsState(bool remoteConnected, bool propagateLoss) {
    m_remoteOverlayActionsEnabled = remoteConnected;

    if (m_screenCanvas) {
        if (!remoteConnected && propagateLoss) {
            m_screenCanvas->handleRemoteConnectionLost();
        }
        m_screenCanvas->setOverlayActionsEnabled(remoteConnected);
    }

    if (!m_uploadButton) return;

    if (m_uploadButtonInOverlay) {
        if (!remoteConnected) {
            m_uploadButton->setEnabled(false);
            m_uploadButton->setCheckable(false);
            m_uploadButton->setChecked(false);
            m_uploadButton->setStyleSheet(ScreenCanvas::overlayDisabledButtonStyle());
            AppColors::applyCanvasButtonFont(m_uploadButtonDefaultFont);
            m_uploadButton->setFont(m_uploadButtonDefaultFont);
            m_uploadButton->setFixedHeight(40);
            m_uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
        }
    } else {
        m_uploadButton->setEnabled(remoteConnected);
    }
}

void CanvasViewPage::showRemoteClientInfo() {
    if (m_remoteClientInfoContainer) {
        m_remoteClientInfoContainer->show();
    }
}

void CanvasViewPage::hideRemoteClientInfo() {
    if (m_remoteClientInfoContainer) {
        m_remoteClientInfoContainer->hide();
    }
}

void CanvasViewPage::addVolumeIndicatorToLayout() {
    if (!m_remoteClientInfoContainer || !m_volumeIndicator) return;
    
    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;
    
    // Check if already in layout
    if (layout->indexOf(m_volumeIndicator) != -1) return;
    
    // Ensure separator exists
    if (!m_remoteInfoSep2) {
        m_remoteInfoSep2 = new QFrame();
        m_remoteInfoSep2->setFrameShape(QFrame::VLine);
        m_remoteInfoSep2->setFrameShadow(QFrame::Sunken);
        m_remoteInfoSep2->setStyleSheet(QString("QFrame { color: %1; }")
            .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        m_remoteInfoSep2->setFixedWidth(1);
    }
    
    // Add separator if not present
    if (layout->indexOf(m_remoteInfoSep2) == -1) {
        layout->addWidget(m_remoteInfoSep2);
    }
    
    // Add volume indicator
    layout->addWidget(m_volumeIndicator);
    m_volumeIndicator->show();
}

void CanvasViewPage::removeVolumeIndicatorFromLayout() {
    if (!m_remoteClientInfoContainer || !m_volumeIndicator) return;
    
    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;
    
    int idx = layout->indexOf(m_volumeIndicator);
    if (idx != -1) {
        layout->removeWidget(m_volumeIndicator);
        m_volumeIndicator->setParent(nullptr);
        m_volumeIndicator->hide();
    }
    
    // Also remove separator if present
    if (m_remoteInfoSep2 && m_remoteInfoSep2->parent() == m_remoteClientInfoContainer) {
        int sidx = layout->indexOf(m_remoteInfoSep2);
        if (sidx != -1) {
            layout->removeWidget(m_remoteInfoSep2);
            m_remoteInfoSep2->setParent(nullptr);
            m_remoteInfoSep2->hide();
        }
    }
}

void CanvasViewPage::addRemoteStatusToLayout() {
    if (!m_remoteClientInfoContainer || !m_remoteConnectionStatusLabel) return;
    
    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;
    
    if (!m_remoteInfoSep1) {
        m_remoteInfoSep1 = new QFrame();
        m_remoteInfoSep1->setFrameShape(QFrame::VLine);
        m_remoteInfoSep1->setFrameShadow(QFrame::Sunken);
        m_remoteInfoSep1->setStyleSheet(QString("QFrame { color: %1; }")
            .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        m_remoteInfoSep1->setFixedWidth(1);
    }
    
    // Remove from current position (if any) to re-insert at a fixed index
    int sepIdx = layout->indexOf(m_remoteInfoSep1);
    if (sepIdx != -1) {
        layout->removeWidget(m_remoteInfoSep1);
    }
    int statusIdx = layout->indexOf(m_remoteConnectionStatusLabel);
    if (statusIdx != -1) {
        layout->removeWidget(m_remoteConnectionStatusLabel);
    }

    // Insert after hostname label (m_clientNameLabel) to guarantee order: hostname → sep1 → status
    int baseIdx = 0;
    if (m_clientNameLabel && m_clientNameLabel->parent() == m_remoteClientInfoContainer) {
        int nameIdx = layout->indexOf(m_clientNameLabel);
        if (nameIdx != -1) baseIdx = nameIdx + 1;
    }
    
    layout->insertWidget(baseIdx, m_remoteInfoSep1);
    layout->insertWidget(baseIdx + 1, m_remoteConnectionStatusLabel);
    
    m_remoteInfoSep1->show();
    m_remoteConnectionStatusLabel->show();
}

void CanvasViewPage::removeRemoteStatusFromLayout() {
    if (!m_remoteClientInfoContainer || !m_remoteConnectionStatusLabel) return;
    
    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;
    
    int idx = layout->indexOf(m_remoteConnectionStatusLabel);
    if (idx != -1) {
        layout->removeWidget(m_remoteConnectionStatusLabel);
        m_remoteConnectionStatusLabel->setParent(nullptr);
        m_remoteConnectionStatusLabel->hide();
    }
    
    if (m_remoteInfoSep1 && m_remoteInfoSep1->parent() == m_remoteClientInfoContainer) {
        int sidx = layout->indexOf(m_remoteInfoSep1);
        if (sidx != -1) {
            layout->removeWidget(m_remoteInfoSep1);
            m_remoteInfoSep1->setParent(nullptr);
            m_remoteInfoSep1->hide();
        }
    }
}

void CanvasViewPage::setCanvas(ScreenCanvas* canvas) {
    m_screenCanvas = canvas;
    
    if (!canvas || !m_canvasHostStack) return;
    
    // Add canvas to host stack if not already there
    if (m_canvasHostStack->indexOf(canvas) == -1) {
        m_canvasHostStack->addWidget(canvas);
    }
    
    m_canvasHostStack->setCurrentWidget(canvas);
}

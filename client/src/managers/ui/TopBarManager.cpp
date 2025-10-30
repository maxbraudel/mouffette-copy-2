#include "managers/ui/TopBarManager.h"
#include "ui/widgets/ClippedContainer.h"
#include "ui/theme/StyleConfig.h"
#include "core/AppColors.h"
#include <QLabel>
#include <QHBoxLayout>
#include <QFrame>

// [Phase 17] Style configuration now accessed via macros from StyleConfig.h

TopBarManager::TopBarManager(QObject* parent)
    : QObject(parent)
{
}

TopBarManager::~TopBarManager() = default;

void TopBarManager::createLocalClientInfoContainer() {
    if (m_localClientInfoContainer) {
        return; // Already created
    }
    
    // Create "You" title label first
    m_localClientTitleLabel = new QLabel("You");
    m_localClientTitleLabel->setStyleSheet(
        QString("QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    padding: 0px %1px; "
        "    font-size: 16px; "
        "    font-weight: bold; "
        "    color: palette(text); "
        "}").arg(gRemoteClientContainerPadding)
    );
    m_localClientTitleLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    
    // Create network status label
    m_localNetworkStatusLabel = new QLabel("DISCONNECTED");
    // Make status label non-shrinkable with fixed width
    m_localNetworkStatusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_localNetworkStatusLabel->setFixedWidth(120); // Fixed width for consistency
    m_localNetworkStatusLabel->setAlignment(Qt::AlignCenter); // Center the text
    // Note: Color and font styling will be applied by setLocalNetworkStatus()
    
    // Create container widget with same styling as remote client container and proper clipping
    m_localClientInfoContainer = new ClippedContainer();
    
    // Apply same dynamic box styling as remote container with clipping
    const QString containerStyle = QString(
        "QWidget { "
        "    background-color: transparent; "
        "    color: palette(button-text); "
        "    border: 1px solid %3; "
        "    border-radius: %1px; "
        "    min-height: %2px; "
        "    max-height: %2px; "
        "}"
    ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource));
    
    m_localClientInfoContainer->setStyleSheet(containerStyle);
    m_localClientInfoContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_localClientInfoContainer->setMinimumWidth(120); // Same minimum as remote container
    
    // Create horizontal layout for the container
    QHBoxLayout* containerLayout = new QHBoxLayout(m_localClientInfoContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0); // No padding at all
    containerLayout->setSpacing(0); // We'll add separators manually
    
    // Add "You" title (same styling as hostname in remote container)
    containerLayout->addWidget(m_localClientTitleLabel);
    
    // Add vertical separator
    QFrame* separator = new QFrame();
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    separator->setFixedWidth(1);
    containerLayout->addWidget(separator);
    
    // Add network status (styling will be applied by setLocalNetworkStatus())
    containerLayout->addWidget(m_localNetworkStatusLabel);
}

QWidget* TopBarManager::getLocalClientInfoContainer() const {
    return m_localClientInfoContainer;
}

QLabel* TopBarManager::getLocalNetworkStatusLabel() const {
    return m_localNetworkStatusLabel;
}

void TopBarManager::setLocalNetworkStatus(const QString& status) {
    if (!m_localNetworkStatusLabel) return;
    
    const QString up = status.toUpper();
    m_localNetworkStatusLabel->setText(up);
    
    // Apply same styling as remote connection status
    QString textColor, bgColor;
    if (up == "CONNECTED") {
        textColor = AppColors::colorToCss(AppColors::gStatusConnectedText);
        bgColor = AppColors::colorToCss(AppColors::gStatusConnectedBg);
    } else if (up == "ERROR" || up.startsWith("CONNECTING") || up.startsWith("RECONNECTING")) {
        textColor = AppColors::colorToCss(AppColors::gStatusWarningText);
        bgColor = AppColors::colorToCss(AppColors::gStatusWarningBg);
    } else {
        textColor = AppColors::colorToCss(AppColors::gStatusErrorText);
        bgColor = AppColors::colorToCss(AppColors::gStatusErrorBg);
    }
    
    m_localNetworkStatusLabel->setStyleSheet(
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
}

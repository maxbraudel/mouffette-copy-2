#include "frontend/managers/ui/RemoteClientInfoManager.h"
#include "frontend/ui/theme/AppColors.h"
#include "frontend/ui/theme/StyleConfig.h"
#include "frontend/ui/widgets/ClippedContainer.h"
#include <QHBoxLayout>
#include <QFrame>

// [Phase 17] Style configuration now accessed via macros from StyleConfig.h

RemoteClientInfoManager::RemoteClientInfoManager(QObject* parent)
    : QObject(parent)
{
}

QWidget* RemoteClientInfoManager::getContainer() const {
    return m_remoteClientInfoContainer;
}

void RemoteClientInfoManager::ensureLabelsExist() {
    if (!m_clientNameLabel) {
        m_clientNameLabel = new QLabel();
        m_clientNameLabel->setStyleSheet(QString(
            "QLabel { "
            "    background: transparent; "
            "    border: none; "
            "    padding: 0px %1px; "
            "    font-size: 16px; "
            "    font-weight: bold; "
            "    color: palette(text); "
            "}").arg(gRemoteClientContainerPadding)
        );
        m_clientNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_clientNameLabel->setMinimumWidth(20);
    }

    if (!m_remoteConnectionStatusLabel) {
        m_remoteConnectionStatusLabel = new QLabel("DISCONNECTED");
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
        m_remoteConnectionStatusLabel->setFixedWidth(120);
        m_remoteConnectionStatusLabel->setAlignment(Qt::AlignCenter);
    }

    if (!m_volumeIndicator) {
        m_volumeIndicator = new QLabel("🔈 --");
        m_volumeIndicator->setStyleSheet(
            QString("QLabel { "
            "    background: transparent; "
            "    border: none; "
            "    padding: 0px %1px; "
            "    font-size: 16px; "
            "    font-weight: bold; "
            "}").arg(gRemoteClientContainerPadding)
        );
        m_volumeIndicator->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_volumeIndicator->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
}

void RemoteClientInfoManager::createContainer() {
    if (m_remoteClientInfoContainer) {
        return; // Already created
    }

    // Ensure labels exist
    ensureLabelsExist();

    // Create container widget with dynamic box styling and proper clipping
    m_remoteClientInfoContainer = new ClippedContainer();

    // Apply dynamic box styling with transparent background and clipping
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

    m_remoteClientInfoContainer->setStyleSheet(containerStyle);
    m_remoteClientInfoContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_remoteClientInfoContainer->setMinimumWidth(120);

    // Create horizontal layout for the container
    QHBoxLayout* containerLayout = new QHBoxLayout(m_remoteClientInfoContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    // Add hostname
    containerLayout->addWidget(m_clientNameLabel);

    // Add vertical separator
    m_remoteInfoSep1 = new QFrame();
    m_remoteInfoSep1->setFrameShape(QFrame::VLine);
    m_remoteInfoSep1->setFrameShadow(QFrame::Sunken);
    m_remoteInfoSep1->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    m_remoteInfoSep1->setFixedWidth(1);
    containerLayout->addWidget(m_remoteInfoSep1);

    // Add status
    containerLayout->addWidget(m_remoteConnectionStatusLabel);

    // Add vertical separator
    m_remoteInfoSep2 = new QFrame();
    m_remoteInfoSep2->setFrameShape(QFrame::VLine);
    m_remoteInfoSep2->setFrameShadow(QFrame::Sunken);
    m_remoteInfoSep2->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    m_remoteInfoSep2->setFixedWidth(1);
    containerLayout->addWidget(m_remoteInfoSep2);

    // Add volume indicator
    containerLayout->addWidget(m_volumeIndicator);
}

void RemoteClientInfoManager::updateClientNameDisplay(const ClientInfo& client) {
    if (!m_clientNameLabel) return;

    QString name = client.getMachineName().trimmed();
    QString platform = client.getPlatform().trimmed();

    if (name.isEmpty()) {
        name = QObject::tr("Unknown Machine");
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

void RemoteClientInfoManager::updateVolumeIndicator(int volumePercent) {
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
}

void RemoteClientInfoManager::removeRemoteStatusFromLayout() {
    if (!m_remoteClientInfoContainer || !m_remoteConnectionStatusLabel) return;

    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;

    // Remove status label
    int idx = layout->indexOf(m_remoteConnectionStatusLabel);
    if (idx != -1) {
        layout->removeWidget(m_remoteConnectionStatusLabel);
        m_remoteConnectionStatusLabel->setParent(nullptr);
        m_remoteConnectionStatusLabel->hide();
    }

    // Remove separator
    if (m_remoteInfoSep1 && m_remoteInfoSep1->parent() == m_remoteClientInfoContainer) {
        int sidx = layout->indexOf(m_remoteInfoSep1);
        if (sidx != -1) {
            layout->removeWidget(m_remoteInfoSep1);
            m_remoteInfoSep1->setParent(nullptr);
            m_remoteInfoSep1->hide();
        }
    }
}

void RemoteClientInfoManager::addRemoteStatusToLayout() {
    if (!m_remoteClientInfoContainer || !m_remoteConnectionStatusLabel) return;

    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;

    // Ensure separator exists
    if (!m_remoteInfoSep1) {
        m_remoteInfoSep1 = new QFrame();
        m_remoteInfoSep1->setFrameShape(QFrame::VLine);
        m_remoteInfoSep1->setFrameShadow(QFrame::Sunken);
        m_remoteInfoSep1->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        m_remoteInfoSep1->setFixedWidth(1);
    }

    // Remove from current position (if any) to re-insert at fixed index
    int sepIdx = layout->indexOf(m_remoteInfoSep1);
    if (sepIdx != -1) {
        layout->removeWidget(m_remoteInfoSep1);
    }
    int statusIdx = layout->indexOf(m_remoteConnectionStatusLabel);
    if (statusIdx != -1) {
        layout->removeWidget(m_remoteConnectionStatusLabel);
    }

    // Insert after hostname label to guarantee order: hostname → sep1 → status
    int baseIdx = 0;
    if (m_clientNameLabel && m_clientNameLabel->parent() == m_remoteClientInfoContainer) {
        int nameIdx = layout->indexOf(m_clientNameLabel);
        if (nameIdx != -1) baseIdx = nameIdx + 1;
    }

    layout->insertWidget(baseIdx, m_remoteInfoSep1);
    m_remoteInfoSep1->setParent(m_remoteClientInfoContainer);
    m_remoteInfoSep1->show();

    layout->insertWidget(baseIdx + 1, m_remoteConnectionStatusLabel);
    m_remoteConnectionStatusLabel->setParent(m_remoteClientInfoContainer);
    m_remoteConnectionStatusLabel->show();
}

void RemoteClientInfoManager::removeVolumeIndicatorFromLayout() {
    if (!m_remoteClientInfoContainer || !m_volumeIndicator) return;

    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;

    // Remove volume label
    int idx = layout->indexOf(m_volumeIndicator);
    if (idx != -1) {
        layout->removeWidget(m_volumeIndicator);
        m_volumeIndicator->setParent(nullptr);
        m_volumeIndicator->hide();
    }

    // Remove separator
    if (m_remoteInfoSep2 && m_remoteInfoSep2->parent() == m_remoteClientInfoContainer) {
        int sidx = layout->indexOf(m_remoteInfoSep2);
        if (sidx != -1) {
            layout->removeWidget(m_remoteInfoSep2);
            m_remoteInfoSep2->setParent(nullptr);
            m_remoteInfoSep2->hide();
        }
    }
}

void RemoteClientInfoManager::addVolumeIndicatorToLayout() {
    if (!m_remoteClientInfoContainer || !m_volumeIndicator) return;

    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;

    // Ensure separator exists
    if (!m_remoteInfoSep2) {
        m_remoteInfoSep2 = new QFrame();
        m_remoteInfoSep2->setFrameShape(QFrame::VLine);
        m_remoteInfoSep2->setFrameShadow(QFrame::Sunken);
        m_remoteInfoSep2->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        m_remoteInfoSep2->setFixedWidth(1);
    }

    // Remove current instances (if any) to reinsert at fixed index
    int sepIdx = layout->indexOf(m_remoteInfoSep2);
    if (sepIdx != -1) {
        layout->removeWidget(m_remoteInfoSep2);
    }
    int volIdx = layout->indexOf(m_volumeIndicator);
    if (volIdx != -1) {
        layout->removeWidget(m_volumeIndicator);
    }

    // Insert after status if present; otherwise after hostname
    int baseIdx = -1;
    if (m_remoteConnectionStatusLabel && m_remoteConnectionStatusLabel->parent() == m_remoteClientInfoContainer) {
        baseIdx = layout->indexOf(m_remoteConnectionStatusLabel);
    }
    if (baseIdx == -1 && m_clientNameLabel && m_clientNameLabel->parent() == m_remoteClientInfoContainer) {
        baseIdx = layout->indexOf(m_clientNameLabel);
    }
    if (baseIdx == -1) baseIdx = layout->count() - 1;

    // Guarantee order: ... status → sep2 → volume
    layout->insertWidget(baseIdx + 1, m_remoteInfoSep2);
    m_remoteInfoSep2->setParent(m_remoteClientInfoContainer);
    m_remoteInfoSep2->show();

    layout->insertWidget(baseIdx + 2, m_volumeIndicator);
    m_volumeIndicator->setParent(m_remoteClientInfoContainer);
    m_volumeIndicator->show();
}

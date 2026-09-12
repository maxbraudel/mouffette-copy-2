#include "frontend/ui/pages/HistoryPage.h"

#include "backend/notifications/HistoryStore.h"
#include "backend/notifications/NotificationCenter.h"
#include "frontend/ui/theme/AppColors.h"
#include "frontend/ui/theme/ThemeManager.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPair>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
QString severityLabel(NotificationSeverity severity)
{
    switch (severity) {
    case NotificationSeverity::Success: return QStringLiteral("SUCCESS");
    case NotificationSeverity::Error: return QStringLiteral("ERROR");
    case NotificationSeverity::Warning: return QStringLiteral("WARNING");
    case NotificationSeverity::Info: return QStringLiteral("INFO");
    case NotificationSeverity::Loading: return QStringLiteral("IN PROGRESS");
    }
    return QStringLiteral("INFO");
}

QPair<QColor, QColor> severityColors(NotificationSeverity severity)
{
    switch (severity) {
    case NotificationSeverity::Success:
        return {AppColors::gStatusConnectedText, AppColors::gStatusConnectedBg};
    case NotificationSeverity::Error:
        return {AppColors::gStatusErrorText, AppColors::gStatusErrorBg};
    case NotificationSeverity::Warning:
        return {AppColors::gStatusWarningText, AppColors::gStatusWarningBg};
    case NotificationSeverity::Info:
    case NotificationSeverity::Loading:
        return {AppColors::gBrandBlue, AppColors::gBrandBlueLight};
    }
    return {AppColors::gBrandBlue, AppColors::gBrandBlueLight};
}

QString formattedTimestamp(qint64 timestampMs)
{
    const QDateTime timestamp = QDateTime::fromMSecsSinceEpoch(timestampMs).toLocalTime();
    const QLocale english(QLocale::English);
    return english.toString(timestamp, QStringLiteral("MMM d, yyyy  h:mm:ss AP"));
}
}

HistoryPage::HistoryPage(NotificationCenter* notificationCenter, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HistoryPage"));
    setupUi();
    setNotificationCenter(notificationCenter);
}

void HistoryPage::setupUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    auto* actionsLayout = new QHBoxLayout();
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(12);

    auto* description = new QLabel(
        QStringLiteral("Latest notifications are shown first."), this);
    description->setTextFormat(Qt::PlainText);
    description->setObjectName(QStringLiteral("historyDescriptionLabel"));
    description->setStyleSheet(QStringLiteral("QLabel { color: palette(mid); }"));
    actionsLayout->addWidget(description);
    actionsLayout->addStretch();

    m_clearButton = ThemeManager::createPillButton(QStringLiteral("Clear History"), this);
    m_clearButton->setObjectName(QStringLiteral("clearHistoryButton"));
    m_clearButton->setAccessibleName(QStringLiteral("Clear notification history"));
    const int clearWidth = m_clearButton->fontMetrics().horizontalAdvance(m_clearButton->text()) + 24;
    m_clearButton->setFixedWidth(qMax(110, clearWidth));
    connect(m_clearButton, &QPushButton::clicked,
            this, &HistoryPage::confirmClearHistory);
    actionsLayout->addWidget(m_clearButton);
    rootLayout->addLayout(actionsLayout);

    m_contentStack = new QStackedWidget(this);
    m_contentStack->setObjectName(QStringLiteral("historyContentStack"));

    m_emptyPage = new QWidget(m_contentStack);
    auto* emptyLayout = new QVBoxLayout(m_emptyPage);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    m_emptyLabel = new QLabel(
        QStringLiteral("No notifications yet.\nNew notifications will appear here."),
        m_emptyPage);
    m_emptyLabel->setTextFormat(Qt::PlainText);
    m_emptyLabel->setObjectName(QStringLiteral("historyEmptyStateLabel"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(mid); font-size: 16px; font-style: italic; }"));
    emptyLayout->addStretch();
    emptyLayout->addWidget(m_emptyLabel);
    emptyLayout->addStretch();
    m_contentStack->addWidget(m_emptyPage);

    m_scrollArea = new QScrollArea(m_contentStack);
    m_scrollArea->setObjectName(QStringLiteral("historyScrollArea"));
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { border: none; background: transparent; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"));

    m_entriesHost = new QWidget(m_scrollArea);
    m_entriesHost->setObjectName(QStringLiteral("historyEntriesContainer"));
    m_entriesHost->setStyleSheet(QStringLiteral("background: transparent;"));
    m_entriesLayout = new QVBoxLayout(m_entriesHost);
    m_entriesLayout->setContentsMargins(0, 0, 6, 0);
    m_entriesLayout->setSpacing(8);
    m_scrollArea->setWidget(m_entriesHost);
    m_contentStack->addWidget(m_scrollArea);

    rootLayout->addWidget(m_contentStack, 1);
}

void HistoryPage::setNotificationCenter(NotificationCenter* notificationCenter)
{
    if (m_notificationCenter == notificationCenter) {
        refresh();
        return;
    }
    if (m_notificationCenter) {
        QObject::disconnect(m_notificationCenter, nullptr, this, nullptr);
    }
    m_notificationCenter = notificationCenter;
    if (m_notificationCenter) {
        connect(m_notificationCenter, &NotificationCenter::historyChanged,
                this, &HistoryPage::refresh);
    }
    refresh();
}

QWidget* HistoryPage::createEntryCard(const NotificationEntry& entry) const
{
    auto* card = new QFrame(m_entriesHost);
    card->setObjectName(QStringLiteral("historyEntryCard"));
    card->setFrameShape(QFrame::NoFrame);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    card->setStyleSheet(QString(
        "QFrame#historyEntryCard { border: 1px solid %1; border-radius: 8px; "
        "background-color: %2; }")
        .arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource),
             AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource)));

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 10, 12, 11);
    cardLayout->setSpacing(7);

    auto* metadataLayout = new QHBoxLayout();
    metadataLayout->setContentsMargins(0, 0, 0, 0);
    metadataLayout->setSpacing(8);

    const auto [severityTextColor, severityBackgroundColor] = severityColors(entry.severity);
    auto* severity = new QLabel(severityLabel(entry.severity), card);
    severity->setTextFormat(Qt::PlainText);
    severity->setObjectName(QStringLiteral("historyEntrySeverity"));
    severity->setAlignment(Qt::AlignCenter);
    severity->setStyleSheet(QString(
        "QLabel { color: %1; background-color: %2; border: none; border-radius: 5px; "
        "padding: 3px 7px; font-size: 11px; font-weight: bold; }")
        .arg(AppColors::colorToCss(severityTextColor),
             AppColors::colorToCss(severityBackgroundColor)));
    metadataLayout->addWidget(severity);

    const QString categoryText = entry.category.trimmed().isEmpty()
        ? QStringLiteral("General") : entry.category.trimmed();
    auto* category = new QLabel(categoryText, card);
    category->setTextFormat(Qt::PlainText);
    category->setObjectName(QStringLiteral("historyEntryCategory"));
    category->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(text); font-weight: bold; border: none; background: transparent; }"));
    metadataLayout->addWidget(category);
    metadataLayout->addStretch();

    auto* timestamp = new QLabel(formattedTimestamp(entry.timestampMs), card);
    timestamp->setTextFormat(Qt::PlainText);
    timestamp->setObjectName(QStringLiteral("historyEntryTimestamp"));
    timestamp->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timestamp->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(mid); border: none; background: transparent; }"));
    metadataLayout->addWidget(timestamp);
    cardLayout->addLayout(metadataLayout);

    auto* message = new QLabel(entry.message, card);
    // Notification text may include peer/server diagnostics. Never let QLabel
    // auto-detect it as rich text or resolve embedded resources.
    message->setTextFormat(Qt::PlainText);
    message->setObjectName(QStringLiteral("historyEntryMessage"));
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    message->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    message->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(text); border: none; background: transparent; }"));
    cardLayout->addWidget(message);

    card->setAccessibleName(QStringLiteral("%1 notification from %2")
                                .arg(severityLabel(entry.severity), categoryText));
    card->setAccessibleDescription(entry.message);
    return card;
}

void HistoryPage::refresh()
{
    while (QLayoutItem* item = m_entriesLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            delete widget;
        }
        delete item;
    }

    const QList<NotificationEntry> entries = m_notificationCenter
        ? m_notificationCenter->entries() : QList<NotificationEntry>();
    for (const NotificationEntry& entry : entries) {
        m_entriesLayout->addWidget(createEntryCard(entry));
    }
    m_entriesLayout->addStretch(1);

    const bool empty = entries.isEmpty();
    m_contentStack->setCurrentWidget(empty ? m_emptyPage : m_scrollArea);
    m_clearButton->setEnabled(!empty && m_notificationCenter);
}

void HistoryPage::confirmClearHistory()
{
    if (!m_notificationCenter || m_notificationCenter->entries().isEmpty()) {
        return;
    }

    QMessageBox confirmation(QMessageBox::Warning,
                             QStringLiteral("Clear History"),
                             QStringLiteral("Clear all notification history?"),
                             QMessageBox::NoButton,
                             this);
    confirmation.setInformativeText(QStringLiteral(
        "This permanently removes every saved notification from this device."));
    QPushButton* clearButton = confirmation.addButton(
        QStringLiteral("Clear History"), QMessageBox::DestructiveRole);
    QPushButton* cancelButton = confirmation.addButton(
        QStringLiteral("Cancel"), QMessageBox::RejectRole);
    confirmation.setDefaultButton(cancelButton);
    confirmation.setEscapeButton(cancelButton);
    confirmation.exec();

    if (confirmation.clickedButton() != clearButton) {
        return;
    }
    if (!m_notificationCenter->clearHistory()) {
        QMessageBox::warning(this,
                             QStringLiteral("History Not Cleared"),
                             m_notificationCenter->lastError().isEmpty()
                                 ? QStringLiteral("The notification history could not be cleared.")
                                 : m_notificationCenter->lastError());
    }
}

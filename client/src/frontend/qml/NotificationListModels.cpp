#include "frontend/qml/NotificationListModels.h"

#include <QDateTime>
#include <QTimer>
#include <QUuid>

namespace {
constexpr int kToastAnimationDurationMs = 300;
}

HistoryListModel::HistoryListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int HistoryListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant HistoryListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const NotificationEntry& entry = m_rows.at(index.row());
    switch (role) {
    case EntryIdRole: return entry.id;
    case SeverityKindRole: return severityKind(entry.severity);
    case SeverityLabelRole: return notificationSeverityToString(entry.severity).toUpper();
    case CategoryRole: return entry.category;
    case MessageRole: return entry.message;
    case TimestampTextRole:
        return QDateTime::fromMSecsSinceEpoch(entry.timestampMs)
            .toString(QStringLiteral("dd/MM/yyyy HH:mm:ss"));
    case TimestampRole: return entry.timestampMs;
    case ReadRole: return entry.read;
    default: return {};
    }
}

QHash<int, QByteArray> HistoryListModel::roleNames() const
{
    return {
        { EntryIdRole, QByteArrayLiteral("entryId") },
        { SeverityKindRole, QByteArrayLiteral("severityKind") },
        { SeverityLabelRole, QByteArrayLiteral("severityLabel") },
        { CategoryRole, QByteArrayLiteral("category") },
        { MessageRole, QByteArrayLiteral("message") },
        { TimestampTextRole, QByteArrayLiteral("timestampText") },
        { TimestampRole, QByteArrayLiteral("timestamp") },
        { ReadRole, QByteArrayLiteral("read") }
    };
}

void HistoryListModel::setSource(NotificationCenter* source)
{
    if (m_source == source) return;
    if (m_source) disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        connect(m_source, &NotificationCenter::historyChanged,
                this, &HistoryListModel::reload);
        connect(m_source, &NotificationCenter::historyCleared,
                this, &HistoryListModel::reload);
    }
    reload();
}

void HistoryListModel::reload()
{
    beginResetModel();
    m_rows = m_source ? m_source->entries() : QList<NotificationEntry>{};
    endResetModel();
}

int HistoryListModel::severityKind(NotificationSeverity severity)
{
    switch (severity) {
    case NotificationSeverity::Success: return 0;
    case NotificationSeverity::Error: return 1;
    case NotificationSeverity::Warning: return 2;
    case NotificationSeverity::Info:
    case NotificationSeverity::Loading: return 3;
    }
    return 3;
}

ToastListModel::ToastListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ToastListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant ToastListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Toast& toast = m_rows.at(index.row());
    switch (role) {
    case ToastIdRole: return toast.id;
    case SeverityKindRole: return toast.severityKind;
    case MessageRole: return toast.message;
    case DismissingRole: return toast.dismissing;
    default: return {};
    }
}

QHash<int, QByteArray> ToastListModel::roleNames() const
{
    return {
        { ToastIdRole, QByteArrayLiteral("toastId") },
        { SeverityKindRole, QByteArrayLiteral("severityKind") },
        { MessageRole, QByteArrayLiteral("message") },
        { DismissingRole, QByteArrayLiteral("dismissing") }
    };
}

void ToastListModel::setSource(NotificationCenter* source)
{
    if (m_source == source) return;
    if (m_source) disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        connect(m_source, &NotificationCenter::toastRequested,
                this, &ToastListModel::appendToast);
    }
}

void ToastListModel::appendToast(const QString& message,
                                 NotificationSeverity severity,
                                 int durationMs)
{
    if (message.isEmpty()) return;
    if (m_rows.size() >= 10) {
        beginRemoveRows(QModelIndex(), 0, 0);
        m_rows.removeFirst();
        endRemoveRows();
    }

    Toast toast;
    toast.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    toast.message = message;
    toast.severityKind = severityKind(severity);
    const int row = m_rows.size();
    beginInsertRows(QModelIndex(), row, row);
    m_rows.append(toast);
    endInsertRows();

    const int lifetime = durationMs > 0 ? durationMs : 4000;
    // The legacy timer started after its 300 ms entrance animation.
    QTimer::singleShot(lifetime + kToastAnimationDurationMs,
                       this, [this, id = toast.id]() {
        beginDismissToast(id);
    });
}

void ToastListModel::beginDismissToast(const QString& id)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        Toast& toast = m_rows[row];
        if (toast.id != id || toast.dismissing) continue;
        toast.dismissing = true;
        const QModelIndex item = index(row);
        emit dataChanged(item, item, {DismissingRole});
        QTimer::singleShot(kToastAnimationDurationMs, this,
                           [this, id]() { removeToast(id); });
        return;
    }
}

void ToastListModel::removeToast(const QString& id)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).id != id) continue;
        beginRemoveRows(QModelIndex(), row, row);
        m_rows.remove(row);
        endRemoveRows();
        return;
    }
}

int ToastListModel::severityKind(NotificationSeverity severity)
{
    switch (severity) {
    case NotificationSeverity::Success: return 0;
    case NotificationSeverity::Error: return 1;
    case NotificationSeverity::Warning: return 2;
    case NotificationSeverity::Info:
    case NotificationSeverity::Loading: return 3;
    }
    return 3;
}

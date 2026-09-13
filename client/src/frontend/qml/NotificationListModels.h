#ifndef NOTIFICATIONLISTMODELS_H
#define NOTIFICATIONLISTMODELS_H

#include <QAbstractListModel>
#include <QVector>

#include "backend/notifications/NotificationCenter.h"

class HistoryListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        EntryIdRole = Qt::UserRole + 1,
        SeverityKindRole,
        SeverityLabelRole,
        CategoryRole,
        MessageRole,
        TimestampTextRole,
        TimestampRole,
        ReadRole
    };

    explicit HistoryListModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSource(NotificationCenter* source);

private:
    void reload();
    static int severityKind(NotificationSeverity severity);

    NotificationCenter* m_source = nullptr;
    QList<NotificationEntry> m_rows;
};

class ToastListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ToastIdRole = Qt::UserRole + 1,
        SeverityKindRole,
        MessageRole
    };

    explicit ToastListModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSource(NotificationCenter* source);

private:
    struct Toast {
        QString id;
        QString message;
        int severityKind = 3;
    };

    void appendToast(const QString& message, NotificationSeverity severity,
                     int durationMs);
    void removeToast(const QString& id);
    static int severityKind(NotificationSeverity severity);

    NotificationCenter* m_source = nullptr;
    QVector<Toast> m_rows;
};

#endif // NOTIFICATIONLISTMODELS_H

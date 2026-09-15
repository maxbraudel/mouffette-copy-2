#ifndef SCENEACTIVITYLISTMODEL_H
#define SCENEACTIVITYLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QTimer>

#include "backend/domain/scene/SceneActivityModel.h"

class ClientListModel;

class SceneActivityListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        SceneRunIdRole = Qt::UserRole + 1,
        PrimaryTextRole,
        SecondaryTextRole,
        BadgeTextRole,
        BadgeKindRole,
        SelectableRole,
        DirectionRole,
        PeerEndpointIdRole,
        StartedAtRole,
        DegradedRole,
        HasProjectRole,
        IdentifierRole
    };

    explicit SceneActivityListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setSource(SceneActivityModel* source);
    void setClientsModel(ClientListModel* clientsModel);

private:
    void reload();
    QString peerDisplayName(const QString& endpointId) const;
    static QString formatDuration(qint64 elapsedMs);

    SceneActivityModel* m_source = nullptr;
    ClientListModel* m_clientsModel = nullptr;
    QList<SceneActivityModel::Activity> m_rows;
    QTimer m_durationTimer;
};

#endif // SCENEACTIVITYLISTMODEL_H

#ifndef SCENEACTIVITYMODEL_H
#define SCENEACTIVITYMODEL_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class SceneActivityModel final : public QObject
{
    Q_OBJECT

public:
    enum class Direction {
        Outgoing,
        Incoming
    };
    Q_ENUM(Direction)

    struct Activity {
        QString sceneRunId;
        QString remoteSessionId;
        QString ownerDeviceId;
        QString targetDeviceId;
        QString peerDeviceId;
        Direction direction = Direction::Outgoing;
        qint64 startedAtEpochMs = 0;
        bool degraded = false;
    };

    explicit SceneActivityModel(QObject* parent = nullptr);

    void setLocalDeviceId(const QString& deviceId);
    QString localDeviceId() const { return m_localDeviceId; }

    bool upsertLive(const QString& sceneRunId,
                    const QString& remoteSessionId,
                    const QString& ownerDeviceId,
                    const QString& targetDeviceId,
                    qint64 startedAtEpochMs,
                    bool degraded = false);
    bool remove(const QString& sceneRunId);
    int removeForSession(const QString& remoteSessionId);
    void setSessionDegraded(const QString& remoteSessionId, bool degraded);
    void setAllDegraded(bool degraded);
    void clear();

    QList<Activity> liveActivities() const;
    Activity activity(const QString& sceneRunId) const;
    int liveCount() const { return m_activities.size(); }

signals:
    void activitiesChanged();
    void liveCountChanged(int count);

private:
    void publishChange(int previousCount);

    QString m_localDeviceId;
    QHash<QString, Activity> m_activities;
};

Q_DECLARE_METATYPE(SceneActivityModel::Direction)
Q_DECLARE_METATYPE(SceneActivityModel::Activity)

#endif // SCENEACTIVITYMODEL_H

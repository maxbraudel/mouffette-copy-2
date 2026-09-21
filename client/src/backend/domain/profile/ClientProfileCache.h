#pragma once

#include "backend/domain/models/ClientInfo.h"
#include <QCache>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <functional>

// Peer profiles and image derivatives are never serialized. This cache belongs
// to one application runtime; a restarted observer starts with hostname/default.
class ClientProfileCache final : public QObject
{
    Q_OBJECT
public:
    using PictureRequester = std::function<QString(const QString&, const QString&)>;
    explicit ClientProfileCache(QObject* parent = nullptr);
    void setPictureRequester(PictureRequester requester);
    void observe(const QList<ClientInfo>& clients);
    ClientInfo apply(const ClientInfo& client) const;
    QString displayName(const QString& endpoint, const QString& hostname, int ordinal) const;
    QString pictureSource(const QString& endpoint) const;
    void requestPicture(const QString& endpoint);
    void acceptPicture(const QString& requestId, const QString& endpoint,
                       const QString& hash, const QByteArray& jpeg);
    void transportReset();
    void clearPeers();
    void setLocalPicture(const QString& slot, const QByteArray& jpeg);
    QString localPictureSource(const QString& slot) const;
    QImage image(const QString& hash) const;
    static QString defaultSource();
    int imageBytes() const { return m_images.totalCost(); }

signals:
    void profilesChanged();

private:
    struct Peer { QString username; QString hash; bool available = false; quint64 observed = 0; };
    struct Pending { QString endpoint; QString hash; qint64 startedAt = 0; };
    struct Attempt { int count = 0; qint64 retryAt = 0; };
    struct Local { QString hash; QImage image; };
    QHash<QString, Peer> m_peers;
    mutable QCache<QString, QImage> m_images;
    QHash<QString, Local> m_local;
    QHash<QString, Pending> m_pending;
    QHash<QString, Attempt> m_attempts;
    quint64 m_observation = 0;
    PictureRequester m_requester;
    QElapsedTimer m_clock;
};

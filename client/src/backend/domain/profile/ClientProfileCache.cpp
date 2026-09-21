#include "backend/domain/profile/ClientProfileCache.h"
#include "backend/domain/profile/ProfileImage.h"
#include <QTimer>
#include <utility>
#include <limits>

namespace {
QString source(const QString& hash) { return QStringLiteral("image://profiles/%1").arg(hash); }
QString key(const QString& endpoint, const QString& hash) { return endpoint + QLatin1Char('/') + hash; }
}

ClientProfileCache::ClientProfileCache(QObject* parent)
    : QObject(parent), m_images(64 * 1024 * 1024)
{
    m_clock.start();
    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] {
        bool retryReady = false;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (m_clock.elapsed() - it->startedAt >= 35000) {
                Attempt& attempt = m_attempts[key(it->endpoint, it->hash)];
                attempt.retryAt = m_clock.elapsed() + 5000 * attempt.count;
                it = m_pending.erase(it);
            }
            else ++it;
        }
        for (Attempt& attempt : m_attempts) {
            if (attempt.count < 3 && attempt.retryAt > 0 && attempt.retryAt <= m_clock.elapsed()) {
                attempt.retryAt = 0;
                retryReady = true;
            }
        }
        if (retryReady) emit profilesChanged();
    });
    timer->start();
}

void ClientProfileCache::setPictureRequester(PictureRequester requester) { m_requester = std::move(requester); }
QString ClientProfileCache::defaultSource() { return QStringLiteral("qrc:/icons/default-profile-picture.jpg"); }

void ClientProfileCache::observe(const QList<ClientInfo>& clients)
{
    bool changed = false;
    QSet<QString> seen;
    for (const ClientInfo& client : clients) {
        const QString endpoint = client.endpointId();
        seen.insert(endpoint);
        Peer& peer = m_peers[endpoint];
        peer.observed = ++m_observation;
        const bool available = client.hasProfileMetadata() && client.canAcceptSession();
        if (client.hasProfileMetadata() || client.canAcceptSession()) {
            if (peer.username != client.username() || peer.hash != client.profilePictureHash()) {
                m_attempts.remove(key(endpoint, peer.hash));
                for (auto it = m_pending.begin(); it != m_pending.end();) {
                    if (it->endpoint == endpoint) it = m_pending.erase(it);
                    else ++it;
                }
                peer.username = client.username();
                peer.hash = client.profilePictureHash();
                changed = true;
                m_attempts.remove(key(endpoint, peer.hash));
            }
        }
        if (peer.available != available) {
            peer.available = available;
            if (available) m_attempts.remove(key(endpoint, peer.hash));
            changed = true;
        }
    }
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (!seen.contains(it.key()) && it->available) {
            it->available = false;
            changed = true;
        }
    }
    // Bound retained offline metadata independently of the decoded-image LRU.
    while (m_peers.size() > 4096) {
        auto oldest = m_peers.begin();
        for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
            if (it->observed < oldest->observed) oldest = it;
        m_attempts.remove(key(oldest.key(), oldest->hash));
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (it->endpoint == oldest.key()) it = m_pending.erase(it);
            else ++it;
        }
        m_peers.erase(oldest);
    }
    if (changed) emit profilesChanged();
}

ClientInfo ClientProfileCache::apply(const ClientInfo& client) const
{
    ClientInfo result = client;
    const auto found = m_peers.constFind(client.endpointId());
    if (found != m_peers.cend()) {
        result.setUsername(found->username);
        result.setProfilePictureHash(found->hash);
    }
    return result;
}

QString ClientProfileCache::displayName(const QString& endpoint, const QString& hostname, int ordinal) const
{
    ClientInfo client(endpoint, hostname, {});
    client.setInstanceOrdinal(ordinal);
    return apply(client).getInstanceDisplayName();
}

QString ClientProfileCache::pictureSource(const QString& endpoint) const
{
    const QString hash = m_peers.value(endpoint).hash;
    return !hash.isEmpty() && m_images.contains(hash) ? source(hash) : defaultSource();
}

void ClientProfileCache::requestPicture(const QString& endpoint)
{
    const Peer peer = m_peers.value(endpoint);
    const Attempt attempt = m_attempts.value(key(endpoint, peer.hash));
    if (!m_requester || !peer.available || peer.hash.isEmpty() || m_images.contains(peer.hash)
        || attempt.count >= 3 || attempt.retryAt > m_clock.elapsed()) return;
    for (const Pending& pending : std::as_const(m_pending))
        if (pending.hash == peer.hash) return;
    const QString id = m_requester(endpoint, peer.hash);
    if (id.isEmpty()) return;
    m_attempts.insert(key(endpoint, peer.hash), {attempt.count + 1, std::numeric_limits<qint64>::max()});
    m_pending.insert(id, {endpoint, peer.hash, m_clock.elapsed()});
}

void ClientProfileCache::acceptPicture(const QString& requestId, const QString& endpoint,
                                     const QString& hash, const QByteArray& jpeg)
{
    const auto it = m_pending.constFind(requestId);
    if (it == m_pending.cend() || it->endpoint != endpoint || it->hash != hash) return;
    m_pending.remove(requestId);
    if (m_peers.value(endpoint).hash != hash) { emit profilesChanged(); return; }
    QImage decoded;
    if (ProfileImage::hash(jpeg) != hash || !ProfileImage::decodeJpeg(jpeg, &decoded)) {
        Attempt& attempt = m_attempts[key(endpoint, hash)];
        attempt.retryAt = m_clock.elapsed() + 5000 * attempt.count;
        emit profilesChanged();
        return;
    }
    m_images.insert(hash, new QImage(decoded), int(decoded.sizeInBytes()));
    // Successful data may be fetched again if later evicted by the bounded LRU.
    m_attempts.remove(key(endpoint, hash));
    emit profilesChanged();
}

void ClientProfileCache::transportReset()
{
    m_pending.clear();
    m_attempts.clear();
    for (Peer& peer : m_peers) peer.available = false;
    emit profilesChanged();
}

void ClientProfileCache::clearPeers()
{
    m_peers.clear();
    m_images.clear();
    transportReset();
}

void ClientProfileCache::setLocalPicture(const QString& slot, const QByteArray& jpeg)
{
    QImage decoded;
    if (!ProfileImage::decodeJpeg(jpeg, &decoded)) m_local.remove(slot);
    else m_local.insert(slot, {ProfileImage::hash(jpeg), decoded});
}

QString ClientProfileCache::localPictureSource(const QString& slot) const
{
    const QString hash = m_local.value(slot).hash;
    return hash.isEmpty() ? defaultSource() : source(hash);
}

QImage ClientProfileCache::image(const QString& hash) const
{
    for (const Local& local : m_local)
        if (local.hash == hash) return local.image;
    const QImage* cached = m_images.object(hash);
    return cached ? *cached : QImage(QStringLiteral(":/icons/default-profile-picture.jpg"));
}

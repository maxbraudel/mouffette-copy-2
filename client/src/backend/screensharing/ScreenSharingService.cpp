#include "ScreenSharingService.h"
#include "ScreenCaptureSource.h"
#include "ScreenStreamCodec.h"
#include "backend/managers/system/SystemMonitor.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/WebSocketClient.h"

#include <QElapsedTimer>
#include <QDebug>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <utility>

namespace {
QString issueText(const QString& reason)
{
    if (reason == QLatin1String("disabled") || reason == QLatin1String("consent_disabled")
        || reason == QLatin1String("not_allowed"))
        return QObject::tr("Screen sharing is disabled on the remote client.");
    if (reason == QLatin1String("permission_denied"))
        return QObject::tr("Allow screen recording in the remote computer's system settings, then restart the sharing app. See its Settings for details.");
    if (reason == QLatin1String("unavailable") || reason == QLatin1String("capture_error")
        || reason == QLatin1String("error"))
        return QObject::tr("The remote screen could not be captured. See Settings on the sharing client for details.");
    if (reason == QLatin1String("channel_unavailable"))
        return QObject::tr("The remote screen connection is unavailable. Retrying…");
    if (reason == QLatin1String("timeout"))
        return QObject::tr("The remote screen did not respond. Check screen sharing on the remote client.");
    if (reason == QLatin1String("stalled"))
        return QObject::tr("The remote screen stopped updating. Trying to recover the stream…");
    if (reason == QLatin1String("decode_error"))
        return QObject::tr("The remote screen could not be decoded. Trying to recover the stream…");
    return {};
}
}

struct ScreenSharingService::Private {
    ScreenSharingService* q;
    WebSocketClient* network;
    SystemMonitor* monitor;
    QThreadPool pool;
    QTimer timer;
    QElapsedTimer clock;
    bool enabled = false;
    bool suspended = false;
    bool channelReady = false;
    QString viewedEndpoint;
    QString subscribedSession;
    quint64 subscribedGeneration = 0;
    QString receivedStream;
    QString status;
    enum class RemoteState { Unavailable, Loading, Available };
    RemoteState remoteState = RemoteState::Unavailable;
    qint64 waitingSince = -1;
    QSet<QString> reportedIssues;
    QHash<QString, QJsonObject> grants;
    QHash<QString, QHash<int, quint64>> sequences;
    struct Capture {
        ScreenCaptureSource* source = nullptr;
        QPointer<QScreen> screen;
        QRect geometry;
    };
    QHash<int, Capture> captures;
    QSet<int> failedScreens;
    struct Pending {
        QByteArray bytes;
        quint64 sequence = 0;
        QSize size;
        bool keyFrame = false;
        qint64 timestampUs = 0;
    };
    struct Decode {
        ScreenStreamDecoder decoder;
        QQueue<Pending> pending;
        qsizetype pendingBytes = 0;
        quint64 lastSequence = 0;
        quint64 epoch = 0;
        bool waitingForKey = true;
        bool busy = false;
        qint64 lastFrameAt = -1;
        qint64 lastKeyRequestAt = -1;
    };
    QHash<int, std::shared_ptr<Decode>> decoders;

    bool ready(const QString& session, bool incoming) const {
        const auto binding = network->remoteSessionCoordinator()->byId(session);
        return !suspended && network->isConnected() && binding.active
            && binding.phase == QLatin1String("Active")
            && network->canIssueSessionCommands(session)
            && (incoming ? binding.targetEndpointId : binding.ownerEndpointId) == network->endpointId();
    }
    void setStatus(const QString& value) {
        if (status == value) return;
        status = value;
        emit q->statusChanged();
    }
    void setRemoteState(RemoteState state) {
        if (remoteState == state) return;
        remoteState = state;
        if (state == RemoteState::Available) reportedIssues.clear();
        emit q->remoteStateChanged(viewedEndpoint);
    }
    void reportIssue(const QString& reason) {
        if (viewedEndpoint.isEmpty() || suspended) return;
        const QString message = issueText(reason);
        if (message.isEmpty()) return;
        waitingSince = -1;
        setRemoteState(RemoteState::Unavailable);
        if (reportedIssues.contains(reason)) return;
        reportedIssues.insert(reason);
        emit q->remoteIssue(viewedEndpoint, message);
    }
    void waitForFrame() {
        if (remoteState == RemoteState::Available) return;
        if (waitingSince < 0) waitingSince = clock.elapsed();
        setRemoteState(RemoteState::Loading);
    }
    void clearReceived() {
        decoders.clear(); // In-flight results retain their state, never the canvas.
        receivedStream.clear();
        setRemoteState(RemoteState::Unavailable);
        if (!viewedEndpoint.isEmpty()) emit q->framesCleared(viewedEndpoint);
    }
    void stopCaptures() {
        const auto old = captures;
        captures.clear();
        for (const auto& capture : old) delete capture.source;
    }
    void requestKey(int screen, const std::shared_ptr<Decode>& state) {
        const qint64 now = clock.elapsed();
        if (state->lastKeyRequestAt >= 0 && now - state->lastKeyRequestAt < 500) return;
        state->lastKeyRequestAt = now;
        network->requestScreenShareKeyFrame(subscribedSession, subscribedGeneration, screen);
    }
    void decodeNext(int screen, const std::shared_ptr<Decode>& state) {
        if (state->busy || state->pending.isEmpty() || decoders.value(screen) != state) return;
        const Pending packet = state->pending.dequeue();
        state->pendingBytes -= packet.bytes.size();
        state->busy = true;
        const quint64 epoch = state->epoch;
        auto* watcher = new QFutureWatcher<QVideoFrame>(q);
        QObject::connect(watcher, &QFutureWatcher<QVideoFrame>::finished, q,
                         [this, watcher, screen, state, epoch, packet] {
            const QVideoFrame frame = watcher->result();
            watcher->deleteLater();
            state->busy = false;
            if (decoders.value(screen) != state) return;
            if (state->epoch == epoch && ready(subscribedSession, false)) {
                if (frame.isValid() && frame.size() == packet.size) {
                    state->lastFrameAt = clock.elapsed();
                    waitingSince = -1;
                    emit q->frameReady(viewedEndpoint, screen, frame);
                    setRemoteState(RemoteState::Available);
                } else {
                    state->pending.clear();
                    state->pendingBytes = 0;
                    state->waitingForKey = true;
                    ++state->epoch;
                    setRemoteState(RemoteState::Unavailable);
                    emit q->framesCleared(viewedEndpoint);
                    reportIssue(QStringLiteral("decode_error"));
                    requestKey(screen, state);
                }
            }
            decodeNext(screen, state);
        });
        watcher->setFuture(QtConcurrent::run(&pool, [state, packet] {
            if (packet.keyFrame) state->decoder.reset();
            QString error;
            return state->decoder.decode(packet.bytes, packet.timestampUs, error);
        }));
    }
    void receive(const QJsonObject& metadata, const QByteArray& bytes) {
        if (metadata.value("remoteSessionId").toString() != subscribedSession
            || metadata.value("streamId").toString() != receivedStream
            || receivedStream.isEmpty() || !ready(subscribedSession, false)) return;
        const int screen = metadata.value("screenId").toInt(-1);
        const QSize size(metadata.value("width").toInt(), metadata.value("height").toInt());
        if (screen < 0 || screen > 1000000 || size.isEmpty() || size.width() > 1920
            || size.height() > 1920 || bytes.size() > ScreenStreamEncoder::MaximumPacketBytes) return;
        if (!decoders.contains(screen) && decoders.size() >= 64) return;
        auto& entry = decoders[screen];
        if (!entry) entry = std::make_shared<Decode>();
        const auto state = entry;
        const auto sequence = quint64(metadata.value("sequence").toDouble());
        const bool key = metadata.value("keyFrame").toBool();
        if (sequence <= state->lastSequence) return;
        // P frames require every predecessor. On loss or overload abandon the
        // chain, fence any in-flight result and restart from a self-contained IDR.
        if ((state->lastSequence && sequence != state->lastSequence + 1)
            || state->pending.size() >= 3
            || state->pendingBytes + bytes.size() > 4 * 1024 * 1024) {
            state->pending.clear();
            state->pendingBytes = 0;
            state->waitingForKey = true;
            ++state->epoch;
        }
        state->lastSequence = sequence;
        if (state->waitingForKey && !key) { requestKey(screen, state); return; }
        if (key) state->waitingForKey = false;
        state->pending.enqueue({bytes, sequence, size, key, clock.nsecsElapsed() / 1000});
        state->pendingBytes += bytes.size();
        decodeNext(screen, state);
    }
    void publish(int screen, const ScreenStreamPacket& packet) {
        if (!enabled || suspended || !channelReady || !captures.contains(screen)
            || monitor->screenCaptureTopology().isEmpty()) return;
        bool dropped = false;
        for (auto it = grants.cbegin(); it != grants.cend(); ++it) {
            if (!ready(it.key(), true)) continue;
            QJsonObject header{{"remoteSessionId", it.key()},
                               {"generation", it->value("generation")},
                               {"streamId", it->value("streamId")}};
            header.insert("screenId", screen);
            header.insert("sequence", double(++sequences[it.key()][screen]));
            header.insert("width", packet.size.width());
            header.insert("height", packet.size.height());
            header.insert("keyFrame", packet.keyFrame);
            header.insert("codec", QStringLiteral("h264"));
            dropped |= !network->sendScreenFrame(header, packet.annexB);
        }
        if (dropped) captures.value(screen).source->requestKeyFrame();
    }
    void refreshCaptures() {
        for (auto it = grants.begin(); it != grants.end();) {
            const auto binding = network->remoteSessionCoordinator()->byId(it.key());
            if (!enabled || !network->isConnected() || binding.targetEndpointId != network->endpointId()
                || double(binding.generation) != it->value("generation").toDouble()) {
                sequences.remove(it.key());
                it = grants.erase(it);
            } else ++it;
        }
        bool hasReadyGrant = false;
        for (auto it = grants.cbegin(); it != grants.cend(); ++it) hasReadyGrant |= ready(it.key(), true);
        if (!hasReadyGrant || !channelReady) {
            stopCaptures();
            if (!enabled) setStatus(QObject::tr("Screen sharing is disabled."));
            else if (failedScreens.isEmpty()) setStatus(QObject::tr("Ready to share when a client connects."));
            return;
        }
        const auto topology = monitor->screenCaptureTopology();
        for (auto it = captures.begin(); it != captures.end();) {
            if (it.key() >= topology.size() || topology[it.key()].screen != it->screen
                || topology[it.key()].advertisedGeometry != it->geometry) {
                delete it->source;
                it = captures.erase(it);
            } else ++it;
        }
        for (int i = 0; i < topology.size() && i < 64; ++i) {
            if (captures.contains(i) || failedScreens.contains(i) || !topology[i].screen) continue;
            auto* source = new ScreenCaptureSource(q);
            captures.insert(i, {source, topology[i].screen, topology[i].advertisedGeometry});
            QObject::connect(source, &ScreenCaptureSource::packetReady, q,
                             [this, i](const ScreenStreamPacket& packet) { publish(i, packet); });
            QObject::connect(source, &ScreenCaptureSource::errorOccurred, q,
                             [this, i, source](ScreenCaptureError code, const QString& error) {
                failedScreens.insert(i);
                source->stop();
                captures.remove(i);
                source->deleteLater();
                setStatus(error);
                const QString reason = code == ScreenCaptureError::PermissionDenied
                    ? QStringLiteral("permission_denied") : QStringLiteral("capture_error");
                qWarning().noquote() << "[ScreenSharing] Screen" << i << reason << error;
                for (auto it = grants.cbegin(); it != grants.cend(); ++it)
                    network->sendScreenShareStatus(it.key(), quint64(it->value("generation").toDouble()),
                                                  reason);
            });
            source->start(topology[i].screen);
        }
        if (!captures.isEmpty() && failedScreens.isEmpty()) setStatus(QObject::tr("Sharing your screens."));
    }
};

ScreenSharingService::ScreenSharingService(WebSocketClient* network, SystemMonitor* monitor, QObject* parent)
    : QObject(parent), d(std::make_unique<Private>())
{
    d->q = this;
    d->network = network;
    d->monitor = monitor;
    d->pool.setMaxThreadCount(2);
    d->pool.setExpiryTimeout(5000);
    d->clock.start();
    connect(&d->timer, &QTimer::timeout, this, &ScreenSharingService::refresh);
    d->timer.start(500);
    connect(network, &WebSocketClient::screenChannelReady, this, [this] {
        d->channelReady = true;
        d->failedScreens.clear();
        // A new channel has new server grants; reassert viewing intent.
        d->subscribedGeneration = 0;
        refresh();
    });
    connect(network, &WebSocketClient::screenChannelUnavailable, this, [this] {
        const auto binding = d->network->remoteSessionCoordinator()->outgoingForPeer(d->viewedEndpoint);
        if (!d->viewedEndpoint.isEmpty() && d->ready(binding.remoteSessionId, false))
            d->reportIssue(QStringLiteral("channel_unavailable"));
        d->channelReady = false;
        d->grants.clear();
        d->sequences.clear();
        d->stopCaptures();
        d->clearReceived();
        d->subscribedGeneration = 0;
    });
    connect(network, &WebSocketClient::screenShareRequestReceived, this, [this](const QJsonObject& request) {
        const QString id = request.value("remoteSessionId").toString();
        if (request.value("enabled").toBool() && d->enabled && d->ready(id, true)) {
            const bool changed = d->grants.value(id).value("streamId") != request.value("streamId");
            d->grants.insert(id, request);
            if (changed) {
                d->failedScreens.clear();
                d->sequences.remove(id);
                for (const auto& capture : std::as_const(d->captures)) capture.source->requestKeyFrame();
            }
        } else { d->grants.remove(id); d->sequences.remove(id); }
        d->refreshCaptures();
    });
    connect(network, &WebSocketClient::screenShareStateReceived, this, [this](const QJsonObject& state) {
        if (state.value("remoteSessionId").toString() != d->subscribedSession) return;
        const QString stream = state.value("streamId").toString();
        const bool enabled = state.value("enabled").toBool();
        const QString reason = state.value("reason").toString();
        const bool wasAvailable = d->remoteState == Private::RemoteState::Available;
        const bool failed = reason == QLatin1String("capture_error") || reason == QLatin1String("permission_denied")
            || reason == QLatin1String("unavailable") || reason == QLatin1String("error");
        if (!enabled || d->receivedStream != stream || failed) d->clearReceived();
        // The video and control sockets may deliver out of order. A delayed
        // keyframe must not resurrect the image after a capture failure.
        d->receivedStream = enabled && !failed ? stream : QString();
        // A channel can be briefly unavailable during the normal handshake.
        // Notify immediately only when an established stream is interrupted;
        // initial connection failures are covered by the first-frame timeout.
        if (reason == QLatin1String("channel_unavailable")) {
            if (wasAvailable) d->reportIssue(reason);
            else if (d->waitingSince >= 0) d->setRemoteState(Private::RemoteState::Loading);
        } else if (!issueText(reason).isEmpty()) {
            d->reportIssue(reason);
        } else if (enabled || reason == QLatin1String("topology_pending")
                   || reason == QLatin1String("topology_changed")) {
            d->waitForFrame();
        }
    });
    connect(network, &WebSocketClient::screenFrameReceived, this,
            [this](const QJsonObject& header, const QByteArray& bytes) { d->receive(header, bytes); });
    connect(network, &WebSocketClient::screenShareKeyFrameRequested, this, [this](const QJsonObject& request) {
        if (!d->grants.contains(request.value("remoteSessionId").toString())) return;
        const int screen = request.value("screenId").toInt(-1);
        for (auto it = d->captures.cbegin(); it != d->captures.cend(); ++it)
            if (screen < 0 || screen == it.key()) it->source->requestKeyFrame();
    });
    connect(network, &WebSocketClient::disconnected, this, &ScreenSharingService::refresh);
    connect(network, &WebSocketClient::remoteSessionOpened, this, &ScreenSharingService::refresh);
    connect(network, &WebSocketClient::remoteSessionResumed, this, &ScreenSharingService::refresh);
    connect(network->remoteSessionCoordinator(), &RemoteSessionCoordinator::sessionChanged,
            this, &ScreenSharingService::refresh);
    connect(network->remoteSessionCoordinator(), &RemoteSessionCoordinator::sessionRemoved,
            this, &ScreenSharingService::refresh);
    connect(monitor, &SystemMonitor::screenTopologyInvalidated, this, [this] {
        d->stopCaptures();
        d->failedScreens.clear();
    });
    connect(monitor, &SystemMonitor::screenConfigurationChanged, this, &ScreenSharingService::refresh);
}

ScreenSharingService::~ScreenSharingService()
{
    d->timer.stop();
    d->stopCaptures();
    d->decoders.clear();
    d->pool.waitForDone();
}

void ScreenSharingService::setSharingEnabled(bool enabled)
{
    const bool changed = d->enabled != enabled;
    d->enabled = enabled;
    if (changed) d->failedScreens.clear();
    d->network->setScreenSharingEnabled(enabled && !d->suspended);
    refresh();
}

void ScreenSharingService::setViewedEndpoint(const QString& endpoint)
{
    if (d->viewedEndpoint == endpoint) return;
    if (!d->subscribedSession.isEmpty())
        d->network->setScreenShareSubscription(d->subscribedSession, d->subscribedGeneration, false);
    d->clearReceived();
    d->subscribedSession.clear();
    d->subscribedGeneration = 0;
    d->waitingSince = -1;
    d->reportedIssues.clear();
    d->viewedEndpoint = endpoint;
    refresh();
}

void ScreenSharingService::setSuspended(bool suspended)
{
    if (d->suspended == suspended) return;
    d->suspended = suspended;
    d->network->setScreenSharingEnabled(d->enabled && !suspended);
    refresh();
}

void ScreenSharingService::refresh()
{
    if (d->network->isConnected() && !d->suspended && (d->enabled || !d->viewedEndpoint.isEmpty()))
        d->network->ensureScreenChannel();
    const auto binding = d->network->remoteSessionCoordinator()->outgoingForPeer(d->viewedEndpoint);
    if (d->viewedEndpoint.isEmpty() || !d->ready(binding.remoteSessionId, false)) {
        d->waitingSince = -1;
        if (!d->subscribedSession.isEmpty()) {
            d->network->setScreenShareSubscription(d->subscribedSession, d->subscribedGeneration, false);
            d->subscribedSession.clear();
            d->subscribedGeneration = 0;
            d->clearReceived();
        }
    } else if (d->subscribedSession != binding.remoteSessionId || d->subscribedGeneration != binding.generation) {
        d->clearReceived();
        if (d->network->setScreenShareSubscription(binding.remoteSessionId, binding.generation, true)) {
            d->subscribedSession = binding.remoteSessionId;
            d->subscribedGeneration = binding.generation;
            d->waitForFrame();
        }
    }
    d->refreshCaptures();
    bool stale = false;
    for (const auto& decoder : std::as_const(d->decoders))
        stale |= decoder->lastFrameAt >= 0 && d->clock.elapsed() - decoder->lastFrameAt > 5000;
    if (stale) {
        d->decoders.clear();
        d->setRemoteState(Private::RemoteState::Unavailable);
        emit framesCleared(d->viewedEndpoint);
        d->reportIssue(QStringLiteral("stalled"));
        d->network->requestScreenShareKeyFrame(d->subscribedSession, d->subscribedGeneration);
    }
    if (d->waitingSince >= 0 && d->clock.elapsed() - d->waitingSince > 10000)
        d->reportIssue(QStringLiteral("timeout"));
}

void ScreenSharingService::stop()
{
    setViewedEndpoint({});
    setSuspended(true);
    d->timer.stop();
}

QString ScreenSharingService::status() const { return d->status; }

bool ScreenSharingService::isRemoteScreenAvailable(const QString& endpoint) const
{
    return !endpoint.isEmpty() && d->viewedEndpoint == endpoint
        && d->remoteState == Private::RemoteState::Available;
}

bool ScreenSharingService::isRemoteScreenLoading(const QString& endpoint) const
{
    return !endpoint.isEmpty() && d->viewedEndpoint == endpoint
        && d->remoteState == Private::RemoteState::Loading;
}

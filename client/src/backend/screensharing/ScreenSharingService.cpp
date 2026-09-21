#include "ScreenSharingService.h"
#include "ScreenCaptureSource.h"
#include "ScreenAdaptiveController.h"
#include "backend/config/AppConfig.h"
#include "backend/network/NetworkDiagnostics.h"
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

namespace {
ScreenAdaptiveController::Limits streamingLimits() {
    const auto& c = AppConfig::instance();
    return {c.screenAdaptiveEnabled(), c.screenMinBitrateKbps() * 1000,
        c.screenInitialBitrateKbps() * 1000, c.screenMaxBitrateKbps() * 1000,
        c.screenUploadBitrateKbps() * 1000, c.screenQueueTargetMs(),
        c.screenFeedbackIntervalMs(), c.screenRecoveryHoldMs()};
}
int requestedEdge(const QJsonObject& grant, int screen) {
    if (!grant.contains("screens")) return 1920; // Legacy receiver safety.
    for (const auto& entry : grant.value("screens").toArray()) {
        const auto item = entry.toObject();
        if (item.value("screenId").toInt(-1) == screen)
            return std::clamp(item.value("maximumEdge").toInt(1920), 160, 3840);
    }
    return 0;
}
ScreenStreamProfile streamProfile(int bitrate, int edge) {
    const auto& c = AppConfig::instance();
    ScreenStreamProfile profile;
    profile.bitrateBps = std::max(32000, bitrate);
    profile.maximumEdge = 320;
    profile.framesPerSecond = 5;
    struct Level { int bitrate; int edge; int fps; };
    static const Level levels[] = {{220000,480,8}, {400000,640,12}, {700000,960,15},
        {1100000,1280,24}, {2200000,1920,30}, {4500000,2560,30},
        {8000000,3840,30}, {16000000,3840,60}};
    for (const auto& level : levels) if (bitrate >= level.bitrate) {
        profile.maximumEdge = level.edge; profile.framesPerSecond = level.fps;
    }
    profile.maximumEdge = std::min({profile.maximumEdge, edge, c.screenMaxEdge()});
    profile.framesPerSecond = std::min(profile.framesPerSecond, c.screenMaxFps());
    profile.idleIntervalMs = c.screenIdleIntervalMs();
    profile.keyFrameIntervalMs = c.screenKeyframeIntervalMs();
    profile.softwarePreset = c.screenSoftwarePreset();
    return profile.normalized();
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
    QJsonArray viewedScreens;
    bool hasViewport = false;
    bool subscriptionDirty = false;
    ScreenAdaptiveController adaptation{streamingLimits()};
    qint64 lastProfileUpdate = -10000;
    qint64 lastDiagnosticsAt = -10000;
    int currentBudget = 0;
    QString subscribedSession;
    quint64 subscribedGeneration = 0;
    QString receivedStream;
    QString status;
    enum class RemoteState { Unavailable, Loading, Available };
    RemoteState remoteState = RemoteState::Unavailable;
    qint64 waitingSince = -1;
    QSet<QString> reportedIssues;
    QSet<int> failedRemoteScreens;
    QHash<int, QSet<QString>> reportedScreenIssues;
    QHash<QString, QJsonObject> grants;
    QHash<QString, QHash<int, quint64>> sequences;
    struct Capture {
        ScreenCaptureSource* source = nullptr;
        QPointer<QScreen> screen;
        QRect geometry;
        ScreenStreamProfile profile;
        int slowEncodes = 0;
        int encodingFailures = 0;
        int recoveryMaximumEdge = ScreenStreamEncoder::MaximumDecodeEdge;
        int recoveryBitrateBps = 100000000;
        int recoveryMaximumFps = 60;
    };
    QHash<int, Capture> captures;
    QSet<int> failedScreens;
    struct Pending {
        QByteArray bytes;
        quint64 sequence = 0;
        QSize size;
        bool keyFrame = false;
        qint64 timestampUs = 0;
        qint64 receivedAt = 0;
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
        int droppedFrames = 0;
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
        if (hasViewport && viewedScreens.isEmpty()) return;
        if (remoteState == RemoteState::Available) return;
        if (waitingSince < 0) waitingSince = clock.elapsed();
        setRemoteState(RemoteState::Loading);
    }
    void clearReceived() {
        decoders.clear(); // In-flight results retain their state, never the canvas.
        failedRemoteScreens.clear();
        reportedScreenIssues.clear();
        receivedStream.clear();
        setRemoteState(RemoteState::Unavailable);
        if (!viewedEndpoint.isEmpty()) emit q->framesCleared(viewedEndpoint);
    }
    void clearRemoteScreen(int screen, const QString& reason) {
        decoders.remove(screen); // Fence an in-flight decode for this screen only.
        emit q->frameCleared(viewedEndpoint, screen);
        bool anyAvailable = false;
        for (const auto& state : std::as_const(decoders))
            anyAvailable |= state->lastFrameAt >= 0
                && clock.elapsed() - state->lastFrameAt <= AppConfig::instance().screenStaleTimeoutMs();
        if (!anyAvailable) {
            waitingSince = -1;
            setRemoteState(RemoteState::Unavailable);
        }
        const QString message = issueText(reason);
        if (message.isEmpty() || reportedScreenIssues[screen].contains(reason)) return;
        reportedScreenIssues[screen].insert(reason);
        emit q->remoteIssue(viewedEndpoint, QObject::tr("Screen %1: %2").arg(screen + 1).arg(message));
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
    bool wantsScreen(int screen) const {
        if (!hasViewport) return true;
        for (const auto& value : viewedScreens)
            if (value.toObject().value("screenId").toInt(-1) == screen) return true;
        return false;
    }
    bool subscribe(const QString& id, quint64 generation) {
        return hasViewport ? network->setScreenShareSubscription(id, generation, true, viewedScreens)
                           : network->setScreenShareSubscription(id, generation, true);
    }
    void updateBackpressure() {
        const bool paused = !network->screenSendWindowOpen();
        for (const auto& capture : std::as_const(captures)) capture.source->setBackpressured(paused);
    }
    void updateProfiles(bool force = false) {
        const qint64 now = clock.elapsed();
        currentBudget = adaptation.budget(now, network->outgoingUploadActive());
        network->setScreenVideoBudget(currentBudget);
        if (!force && now - lastProfileUpdate < 1000) return;
        lastProfileUpdate = now;
        QHash<int, int> edges, copies;
        qint64 weight = 0;
        for (auto it = captures.cbegin(); it != captures.cend(); ++it) {
            int edge = 0, count = 0, receiverCap = 3840;
            for (auto grant = grants.cbegin(); grant != grants.cend(); ++grant) {
                if (!ready(grant.key(), true)) continue;
                const int requested = requestedEdge(*grant, it.key());
                if (requested > 0) {
                    edge = std::max(edge, requested); ++count;
                    receiverCap = std::min(receiverCap, grant->value("receiverMaximumEdge").toInt(1920));
                }
            }
            edge = std::min(edge, receiverCap);
            edges.insert(it.key(), edge); copies.insert(it.key(), count);
            weight += qint64(std::max(160, edge)) * std::max(1, count);
        }
        for (auto it = captures.begin(); it != captures.end(); ++it) {
            const int share = int(qint64(currentBudget) * std::max(160, edges.value(it.key()))
                / std::max<qint64>(1, weight));
            auto profile = streamProfile(share, std::max(160, edges.value(it.key())));
            // Keep an encoder failure's local ceiling for this capture's
            // lifetime. Healthy network feedback must not repeatedly restore
            // a hardware profile that has already failed.
            profile.maximumEdge = std::min(profile.maximumEdge, it->recoveryMaximumEdge);
            profile.bitrateBps = std::min(profile.bitrateBps, it->recoveryBitrateBps);
            profile.framesPerSecond = std::min(profile.framesPerSecond, it->recoveryMaximumFps);
            if (it->profile != profile || force) {
                it->profile = profile;
                it->source->setProfile(profile);
            }
        }
        updateBackpressure();
        if (!captures.isEmpty() && now - lastDiagnosticsAt >= 5000) {
            lastDiagnosticsAt = now;
            NetworkDiagnostics::record(QStringLiteral("screen_quality"), {
                {QStringLiteral("budgetBps"), currentBudget},
                {QStringLiteral("screens"), captures.size()},
                {QStringLiteral("viewers"), grants.size()}});
        }
    }
    void decodeNext(int screen, const std::shared_ptr<Decode>& state) {
        if (state->busy || state->pending.isEmpty() || decoders.value(screen) != state) return;
        if (clock.elapsed() - state->pending.head().receivedAt > AppConfig::instance().screenDecodeQueueMs()) {
            state->droppedFrames += state->pending.size();
            state->pending.clear(); state->pendingBytes = 0;
            state->waitingForKey = true; ++state->epoch;
            requestKey(screen, state);
            network->sendScreenViewFeedback(subscribedSession, subscribedGeneration, screen,
                AppConfig::instance().screenDecodeQueueMs(), state->droppedFrames);
            return;
        }
        const Pending packet = state->pending.dequeue();
        state->pendingBytes -= packet.bytes.size();
        state->busy = true;
        const quint64 epoch = state->epoch;
        const qint64 startedAt = clock.elapsed();
        auto* watcher = new QFutureWatcher<QVideoFrame>(q);
        QObject::connect(watcher, &QFutureWatcher<QVideoFrame>::finished, q,
                         [this, watcher, screen, state, epoch, packet, startedAt] {
            const QVideoFrame frame = watcher->result();
            watcher->deleteLater();
            state->busy = false;
            if (decoders.value(screen) != state) return;
            if (network->sendScreenViewFeedback(subscribedSession, subscribedGeneration, screen,
                    int(clock.elapsed() - startedAt), state->droppedFrames)) state->droppedFrames = 0;
            if (state->epoch == epoch && ready(subscribedSession, false)) {
                if (frame.isValid() && frame.size() == packet.size) {
                    state->lastFrameAt = clock.elapsed();
                    reportedScreenIssues.remove(screen);
                    waitingSince = -1;
                    emit q->frameReady(viewedEndpoint, screen, frame);
                    setRemoteState(RemoteState::Available);
                } else {
                    state->pending.clear();
                    state->pendingBytes = 0;
                    state->waitingForKey = true;
                    ++state->epoch;
                    clearRemoteScreen(screen, QStringLiteral("decode_error"));
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
        if (screen < 0 || screen > 1000000 || failedRemoteScreens.contains(screen)
            || !wantsScreen(screen) || size.isEmpty()
            || size.width() > ScreenStreamEncoder::MaximumDecodeEdge
            || size.height() > ScreenStreamEncoder::MaximumDecodeEdge || bytes.size() > ScreenStreamEncoder::MaximumPacketBytes) return;
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
            state->droppedFrames += state->pending.size() + 1;
            state->pending.clear();
            state->pendingBytes = 0;
            state->waitingForKey = true;
            ++state->epoch;
        }
        state->lastSequence = sequence;
        if (state->waitingForKey && !key) { requestKey(screen, state); return; }
        if (key) state->waitingForKey = false;
        state->pending.enqueue({bytes, sequence, size, key, clock.nsecsElapsed() / 1000, clock.elapsed()});
        state->pendingBytes += bytes.size();
        decodeNext(screen, state);
    }
    void publish(int screen, const ScreenStreamPacket& packet) {
        if (!enabled || suspended || !channelReady || !captures.contains(screen)
            || monitor->screenCaptureTopology().isEmpty()) return;
        bool dropped = false;
        for (auto it = grants.cbegin(); it != grants.cend(); ++it) {
            if (!ready(it.key(), true) || requestedEdge(*it, screen) == 0) continue;
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
        updateBackpressure();
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
        const auto demanded = [this](int screen) {
            for (auto it = grants.cbegin(); it != grants.cend(); ++it)
                if (ready(it.key(), true) && requestedEdge(*it, screen) > 0) return true;
            return false;
        };
        for (const int screen : failedScreens.values())
            if (!demanded(screen)) failedScreens.remove(screen);
        for (auto it = captures.begin(); it != captures.end();) {
            if (!demanded(it.key()) || it.key() >= topology.size() || topology[it.key()].screen != it->screen
                || topology[it.key()].advertisedGeometry != it->geometry) {
                delete it->source;
                it = captures.erase(it);
            } else ++it;
        }
        for (int i = 0; i < topology.size() && i < 64; ++i) {
            if (!demanded(i) || captures.contains(i) || failedScreens.contains(i) || !topology[i].screen) continue;
            auto* source = new ScreenCaptureSource(q);
            captures.insert(i, {source, topology[i].screen, topology[i].advertisedGeometry});
            QObject::connect(source, &ScreenCaptureSource::packetReady, q,
                             [this, i](const ScreenStreamPacket& packet) { publish(i, packet); });
            QObject::connect(source, &ScreenCaptureSource::encodingMeasured, q,
                             [this, i](int elapsedMs) {
                auto it = captures.find(i);
                if (it == captures.end()) return;
                const int limit = std::max(50, 2000 / it->profile.framesPerSecond);
                it->slowEncodes = elapsedMs > limit ? it->slowEncodes + 1 : 0;
                if (it->slowEncodes >= 3) {
                    adaptation.penalize(clock.elapsed());
                    it->slowEncodes = 0;
                }
            });
            QObject::connect(source, &ScreenCaptureSource::errorOccurred, q,
                             [this, i, source](ScreenCaptureError code, const QString& error) {
                auto capture = captures.find(i);
                if (code == ScreenCaptureError::EncodingFailed && capture != captures.end()
                    && ++capture->encodingFailures <= 3 && capture->profile.maximumEdge > 160) {
                    adaptation.penalize(clock.elapsed());
                    auto profile = capture->profile;
                    profile.maximumEdge = std::max(160, (profile.maximumEdge * 3 / 4) & ~1);
                    profile.bitrateBps = std::max(32000, profile.bitrateBps / 2);
                    profile.framesPerSecond = std::max(1, profile.framesPerSecond / 2);
                    capture->recoveryMaximumEdge = profile.maximumEdge;
                    capture->recoveryBitrateBps = profile.bitrateBps;
                    capture->recoveryMaximumFps = profile.framesPerSecond;
                    capture->profile = profile;
                    source->setProfile(profile);
                    source->setBackpressured(!network->screenSendWindowOpen());
                    source->start(capture->screen);
                    return;
                }
                failedScreens.insert(i);
                source->stop();
                captures.remove(i);
                source->deleteLater();
                setStatus(error);
                const QString reason = code == ScreenCaptureError::PermissionDenied
                    ? QStringLiteral("permission_denied") : QStringLiteral("capture_error");
                qWarning().noquote() << "[ScreenSharing] Screen" << i << reason << error;
                for (auto it = grants.cbegin(); it != grants.cend(); ++it)
                    if (requestedEdge(*it, i) > 0)
                        network->sendScreenShareStatus(it.key(), quint64(it->value("generation").toDouble()),
                                                      reason, i);
            });
            updateProfiles(true);
            source->start(topology[i].screen);
        }
        updateProfiles();
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
    d->timer.start(AppConfig::instance().screenFeedbackIntervalMs());
    connect(network, &WebSocketClient::screenSourceFeedback, this, [this](int rtt, bool congested) {
        d->adaptation.observe(QStringLiteral("source"), rtt, congested, false, d->clock.elapsed());
    });
    connect(network, &WebSocketClient::heartbeatSampleReceived, this,
            [this](quint64, qint64 rtt, qint64, qint64) {
        d->adaptation.observe(QStringLiteral("control"), int(std::min<qint64>(60000, rtt)),
                              false, false, d->clock.elapsed());
    });
    connect(network, &WebSocketClient::screenSendWindowChanged, this, [this] { d->updateBackpressure(); });
    connect(network, &WebSocketClient::screenShareFeedbackReceived, this, [this](const QJsonObject& feedback) {
        const QString id = feedback.value("remoteSessionId").toString();
        if (d->grants.value(id).value("streamId") != feedback.value("streamId")) return;
        const QString leg = feedback.value("streamId").toString() + QLatin1Char(':')
            + QString::number(feedback.value("screenId").toInt());
        const int rtt = feedback.value("deliveryRttMs").toInt();
        d->adaptation.observe(leg, rtt > 0 ? rtt : -1,
            feedback.value("congested").toBool(), feedback.value("uploadActive").toBool(), d->clock.elapsed());
    });
    connect(network, &WebSocketClient::screenChannelReady, this, [this] {
        d->channelReady = true;
        d->failedScreens.clear();
        // WebSocketClient replays subscriptions before this signal. A prior
        // channel loss has already invalidated subscribedGeneration; do not
        // overwrite an authoritative disabled response during initial connect.
        refresh();
    });
    connect(network, &WebSocketClient::screenChannelUnavailable, this, [this] {
        const auto binding = d->network->remoteSessionCoordinator()->outgoingForPeer(d->viewedEndpoint);
        if (!d->viewedEndpoint.isEmpty() && d->ready(binding.remoteSessionId, false))
            d->reportIssue(QStringLiteral("channel_unavailable"));
        d->channelReady = false;
        d->adaptation.transportReset(d->clock.elapsed());
        d->grants.clear();
        d->sequences.clear();
        d->stopCaptures();
        d->clearReceived();
        d->subscribedGeneration = 0;
    });
    connect(network, &WebSocketClient::screenShareRequestReceived, this, [this](const QJsonObject& request) {
        const QString id = request.value("remoteSessionId").toString();
        if (request.value("enabled").toBool() && d->enabled && d->ready(id, true)) {
            const auto previous = d->grants.value(id);
            const bool changed = previous.value("streamId") != request.value("streamId");
            const bool demandChanged = previous.value("screens") != request.value("screens");
            d->grants.insert(id, request);
            if (changed) {
                d->failedScreens.clear();
                d->sequences.remove(id);
                for (const auto& capture : std::as_const(d->captures)) capture.source->requestKeyFrame();
            } else if (demandChanged) {
                for (auto capture = d->captures.cbegin(); capture != d->captures.cend(); ++capture)
                    if (requestedEdge(previous, capture.key()) == 0 && requestedEdge(request, capture.key()) > 0)
                        capture->source->requestKeyFrame();
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
        if (state.contains("screenId")) {
            const auto value = state.value("screenId");
            const int screen = value.toInt(-1);
            if (!value.isDouble() || value.toDouble() != screen || screen < 0 || screen > 1000000
                || !enabled || stream != d->receivedStream || !d->wantsScreen(screen)) return;
            if (failed) {
                d->failedRemoteScreens.insert(screen);
                d->clearRemoteScreen(screen, reason);
            } else if (reason == QLatin1String("streaming") || reason == QLatin1String("starting")) {
                d->failedRemoteScreens.remove(screen);
                d->waitForFrame();
            }
            return;
        }
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
    d->hasViewport = false;
    d->viewedScreens = {};
    d->subscriptionDirty = false;
    refresh();
}

void ScreenSharingService::setViewedScreens(const QJsonArray& screens)
{
    if (d->hasViewport && d->viewedScreens == screens) return;
    if (screens.size() > 64) return;
    d->hasViewport = true;
    d->viewedScreens = screens;
    d->subscriptionDirty = true;
    for (const int id : d->decoders.keys()) {
        if (!d->wantsScreen(id)) {
            d->decoders.remove(id);
            emit frameCleared(d->viewedEndpoint, id);
        }
    }
    for (const int id : d->failedRemoteScreens.values()) {
        if (!d->wantsScreen(id)) {
            d->failedRemoteScreens.remove(id);
            d->reportedScreenIssues.remove(id);
        }
    }
    if (screens.isEmpty()) {
        d->waitingSince = -1;
        d->setRemoteState(Private::RemoteState::Unavailable);
    }
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
    } else if (d->subscribedSession != binding.remoteSessionId || d->subscribedGeneration != binding.generation
               || d->subscriptionDirty) {
        if (d->subscribedSession != binding.remoteSessionId || d->subscribedGeneration != binding.generation)
            d->clearReceived();
        if (d->subscribe(binding.remoteSessionId, binding.generation)) {
            d->subscriptionDirty = false;
            d->subscribedSession = binding.remoteSessionId;
            d->subscribedGeneration = binding.generation;
            if (!d->hasViewport || !d->viewedScreens.isEmpty()) d->waitForFrame();
        }
    }
    d->refreshCaptures();
    for (const int screen : d->decoders.keys()) {
        const auto decoder = d->decoders.value(screen);
        if (decoder->lastFrameAt < 0
            || d->clock.elapsed() - decoder->lastFrameAt <= AppConfig::instance().screenStaleTimeoutMs()) continue;
        d->clearRemoteScreen(screen, QStringLiteral("stalled"));
        d->network->requestScreenShareKeyFrame(d->subscribedSession, d->subscribedGeneration, screen);
    }
    if (d->waitingSince >= 0 && d->clock.elapsed() - d->waitingSince > AppConfig::instance().screenFirstFrameTimeoutMs())
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

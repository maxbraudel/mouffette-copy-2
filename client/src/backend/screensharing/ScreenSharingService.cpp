#include "ScreenSharingService.h"
#include "ScreenCaptureSource.h"
#include "ScreenAdaptiveController.h"
#include "ScreenPublicationProfiles.h"
#include "ScreenFrameAdmissionCeiling.h"
#include "ScreenAudioClock.h"
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
#include <cmath>

namespace {
QString issueText(const QString& reason)
{
    if (reason == QLatin1String("disabled") || reason == QLatin1String("consent_disabled")
        || reason == QLatin1String("not_allowed"))
        return QObject::tr("Screen sharing is disabled on the remote client.");
    if (reason == QLatin1String("viewer_capacity") || reason == QLatin1String("capacity_limited"))
        return QObject::tr("The screen sharing capacity limit has been reached. Try again after another viewer disconnects.");
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
ScreenPublicationProfiles::Limits profileLimits() {
    const auto& c = AppConfig::instance();
    return {c.screenMaxEdge(), c.screenMaxFps(), c.screenIdleIntervalMs(),
        c.screenKeyframeIntervalMs(), c.screenSoftwarePreset(), c.screenLowEnabled(),
        c.screenLowMaxEdge(), c.screenLowMaxFps(), c.screenLowMaxBitrateKbps() * 1000,
        c.screenLowMinTotalBitrateKbps() * 1000};
}
ScreenStreamProfile streamProfile(int bitrate, int edge) {
    return ScreenPublicationProfiles::profile(bitrate, edge, profileLimits());
}
}

struct ScreenSharingService::Private {
    ScreenSharingService* q;
    WebSocketClient* network;
    SystemMonitor* monitor;
    QThreadPool pool;
    QTimer timer;
    QTimer presentationTimer;
    ScreenAudioClock audioClock;
    QString audioEndpoint, audioEpoch;
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
    int totalBudget = 0;
    int audioReservation = 0;
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
    QJsonObject publication;
    QHash<int, QHash<QString, quint64>> publicationSequences;
    struct Capture {
        ScreenCaptureSource* source = nullptr;
        QPointer<QScreen> screen;
        QRect geometry;
        ScreenStreamProfile profile;
        QHash<QString, ScreenStreamProfile> layerProfiles;
        QHash<QString, ScreenFrameAdmissionCeiling> admissionCeilings;
        bool lowSuppressed = false;
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
        QVideoFrame presentation;
        qint64 presentationDueUs = 0;
        quint64 presentationEpoch = 0;
    };
    QHash<int, std::shared_ptr<Decode>> decoders;

    void deliverFrame(int screen, const std::shared_ptr<Decode>& state, const QVideoFrame& frame) {
        state->lastFrameAt = clock.elapsed();
        reportedScreenIssues.remove(screen);
        waitingSince = -1;
        emit q->frameReady(viewedEndpoint, screen, frame);
        setRemoteState(RemoteState::Available);
    }
    void presentFrame(int screen, const std::shared_ptr<Decode>& state, const QVideoFrame& frame) {
        const qint64 nowUs = clock.nsecsElapsed() / 1000;
        const qint64 delay = viewedEndpoint == audioEndpoint
            ? audioClock.videoDelayUs(frame.startTime(), nowUs) : 0;
        if (!delay) {
            state->presentation = {};
            deliverFrame(screen, state, frame);
            return;
        }
        // Replacing a waiting image cannot postpone presentation indefinitely.
        state->presentationDueUs = state->presentation.isValid()
            ? std::min(state->presentationDueUs, nowUs + delay) : nowUs + delay;
        state->presentation = frame;
        state->presentationEpoch = state->epoch;
        if (!presentationTimer.isActive()) presentationTimer.start();
    }
    void flushPresentations(bool immediate = false) {
        const auto current = decoders;
        const qint64 nowUs = clock.nsecsElapsed() / 1000;
        bool pending = false;
        for (auto it = current.cbegin(); it != current.cend(); ++it) {
            const auto state = it.value();
            if (!state->presentation.isValid()) continue;
            if (decoders.value(it.key()) != state || state->epoch != state->presentationEpoch
                || !ready(subscribedSession, false)) { state->presentation = {}; continue; }
            if (!immediate && state->presentationDueUs > nowUs) { pending = true; continue; }
            const QVideoFrame frame = std::exchange(state->presentation, {});
            deliverFrame(it.key(), state, frame);
        }
        if (!pending) presentationTimer.stop();
    }

    bool ready(const QString& session, bool incoming) const {
        const auto binding = network->remoteSessionCoordinator()->byId(session);
        return !suspended && network->isConnected() && binding.active
            && binding.phase == QLatin1String("Active")
            && network->canIssueSessionCommands(session)
            && (incoming ? binding.targetEndpointId : binding.ownerEndpointId) == network->endpointId();
    }
    bool shared() const { return network->sharedScreenPublicationSupported(); }
    bool publicationReady() const {
        if (!enabled || suspended || !network->isConnected()
            || !network->isScreenPublicationChannelConnected()
            || !publication.value("enabled").toBool()
            || publication.value("publicationId").toString().isEmpty()) return false;
        // The relay owns the aggregate grant. Retain a local session-lease
        // guard too, without coupling it to any single viewer's session.
        for (const auto& binding : network->remoteSessionCoordinator()->all())
            if (ready(binding.remoteSessionId, true)) return true;
        return false;
    }
    bool lowRequested(int screen) const {
        for (const auto& entry : publication.value("screens").toArray()) {
            const auto item = entry.toObject();
            if (item.value("screenId").toInt(-1) == screen)
                return item.value("layers").toArray().contains(QStringLiteral("low"));
        }
        return false;
    }
    void clearPublication() {
        publication = {};
        publicationSequences.clear();
        stopCaptures();
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
        presentationTimer.stop();
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
        const int budget = adaptation.budget(now, network->outgoingUploadActive());
        if (totalBudget != budget) {
            totalBudget = budget;
            emit q->sourceBudgetChanged(totalBudget);
        }
        currentBudget = std::max(16000, totalBudget - audioReservation);
        network->setScreenVideoBudget(currentBudget);
        if (!force && now - lastProfileUpdate < 1000) return;
        lastProfileUpdate = now;
        QHash<int, int> edges;
        qint64 weight = 0;
        for (auto it = captures.cbegin(); it != captures.cend(); ++it) {
            int edge = 0, count = 0, receiverCap = 3840;
            if (shared()) {
                edge = requestedEdge(publication, it.key());
                count = 1;
            } else for (auto grant = grants.cbegin(); grant != grants.cend(); ++grant) {
                if (!ready(grant.key(), true)) continue;
                const int requested = requestedEdge(*grant, it.key());
                if (requested > 0) {
                    edge = std::max(edge, requested); ++count;
                    receiverCap = std::min(receiverCap, grant->value("receiverMaximumEdge").toInt(1920));
                }
            }
            edge = std::min(edge, receiverCap);
            edges.insert(it.key(), edge);
            weight += qint64(std::max(160, edge)) * std::max(1, count);
        }
        for (auto it = captures.begin(); it != captures.end(); ++it) {
            const int share = int(qint64(currentBudget) * std::max(160, edges.value(it.key()))
                / std::max<qint64>(1, weight));
            const int edge = std::max(160, edges.value(it.key()));
            QHash<QString, ScreenStreamProfile> profiles = shared()
                ? ScreenPublicationProfiles::select(share, edge, lowRequested(it.key()),
                    it->lowSuppressed, it->layerProfiles.contains(QStringLiteral("low")), profileLimits())
                : QHash<QString, ScreenStreamProfile>{{QStringLiteral("main"), streamProfile(share, edge)}};
            // Network recovery cannot restore an encoder profile which failed
            // during this capture's lifetime. Apply its ceilings to both layers.
            for (auto layer = profiles.begin(); layer != profiles.end(); ++layer) {
                auto& profile = layer.value();
                profile = it->admissionCeilings[layer.key()].apply(profile, now);
                profile.maximumEdge = std::min(profile.maximumEdge, it->recoveryMaximumEdge);
                profile.bitrateBps = std::min(profile.bitrateBps, it->recoveryBitrateBps);
                profile.framesPerSecond = std::min(profile.framesPerSecond, it->recoveryMaximumFps);
            }
            if (profiles.contains(QStringLiteral("low"))) {
                const auto main = profiles.value(QStringLiteral("main"));
                const auto low = profiles.value(QStringLiteral("low"));
                if (main.maximumEdge <= low.maximumEdge && main.framesPerSecond <= low.framesPerSecond)
                    profiles.remove(QStringLiteral("low"));
            }
            if (it->layerProfiles != profiles || force) {
                it->layerProfiles = profiles;
                it->profile = profiles.value(QStringLiteral("main"));
                it->source->setProfiles(profiles);
                if (shared()) {
                    const auto id = publication.value("publicationId").toString();
                    for (auto layer = profiles.cbegin(); layer != profiles.cend(); ++layer)
                        network->sendScreenPublicationStatus(id, it.key(), layer.key(), QStringLiteral("starting"),
                            layer->bitrateBps, layer->framesPerSecond);
                    if (lowRequested(it.key()) && !profiles.contains(QStringLiteral("low")))
                        network->sendScreenPublicationStatus(id, it.key(), QStringLiteral("low"), QStringLiteral("inactive"));
                }
            }
        }
        updateBackpressure();
        if (!captures.isEmpty() && now - lastDiagnosticsAt >= 5000) {
            lastDiagnosticsAt = now;
            NetworkDiagnostics::record(QStringLiteral("screen_quality"), {
                {QStringLiteral("budgetBps"), currentBudget},
                {QStringLiteral("screens"), captures.size()},
                {QStringLiteral("sharedPublication"), shared()},
                {QStringLiteral("legacyViewers"), grants.size()}});
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
                    presentFrame(screen, state, frame);
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
        const double timestamp = metadata.value("timestampUs").toDouble(-1);
        const qint64 sourceUs = std::isfinite(timestamp) && timestamp >= 0
            && timestamp <= 9007199254740991.0 ? qint64(timestamp) : -1;
        state->pending.enqueue({bytes, sequence, size, key, sourceUs, clock.elapsed()});
        state->pendingBytes += bytes.size();
        decodeNext(screen, state);
    }
    void publish(int screen, const ScreenStreamPacket& packet) {
        if (!enabled || suspended || !captures.contains(screen)
            || monitor->screenCaptureTopology().isEmpty()) return;
        if (shared()) {
            if (!publicationReady() || requestedEdge(publication, screen) == 0
                || !captures.value(screen).layerProfiles.contains(packet.layer)) return;
            const auto profile = captures.value(screen).layerProfiles.value(packet.layer);
            const QJsonObject header{{"publicationId", publication.value("publicationId")},
                {"screenId", screen}, {"layer", packet.layer},
                {"sequence", double(++publicationSequences[screen][packet.layer])},
                {"width", packet.size.width()}, {"height", packet.size.height()},
                {"keyFrame", packet.keyFrame}, {"codec", QStringLiteral("h264")},
                {"timestampUs", double(packet.timestampUs)},
                {"bitrateBps", profile.bitrateBps}, {"fps", profile.framesPerSecond}};
            if (!network->sendScreenPublicationFrame(header, packet.annexB))
                captures.value(screen).source->requestKeyFrame(packet.layer);
            updateBackpressure();
            return;
        }
        if (!channelReady) return;
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
            header.insert("timestampUs", double(packet.timestampUs));
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
        if (shared() ? !publicationReady() : (!hasReadyGrant || !channelReady)) {
            stopCaptures();
            if (!enabled) setStatus(QObject::tr("Screen sharing is disabled."));
            else if (failedScreens.isEmpty()) setStatus(QObject::tr("Ready to share when a client connects."));
            return;
        }
        const auto topology = monitor->screenCaptureTopology();
        const auto demanded = [this](int screen) {
            if (shared()) return publicationReady() && requestedEdge(publication, screen) > 0;
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
                             [this, i, source](const ScreenStreamPacket& packet) {
                if (captures.value(i).source == source) publish(i, packet);
            });
            QObject::connect(source, &ScreenCaptureSource::encodingMeasured, q,
                             [this, i, source, average = 0.0, samples = 0,
                              warmSince = qint64(-1), overloadedSince = qint64(-1),
                              lastSampleAt = qint64(-1), cooldownUntil = qint64(0),
                              observedProfile = ScreenStreamProfile{}, observedLayers = 0](int elapsedMs) mutable {
                auto it = captures.find(i);
                if (it == captures.end() || it->source != source) return;
                const qint64 now = clock.elapsed();
                const double periodMs = 1000.0 / it->profile.framesPerSecond;
                const int layers = it->layerProfiles.size();
                if (observedProfile != it->profile || observedLayers != layers
                    || lastSampleAt < 0 || now - lastSampleAt > std::max(2000.0, periodMs * 10.0)) {
                    observedProfile = it->profile;
                    observedLayers = layers;
                    average = 0.0;
                    samples = 0;
                    warmSince = now;
                    overloadedSince = -1;
                }
                lastSampleAt = now;
                // Main and low share this worker. Their aggregate conversion
                // time must fit main's frame period, with 20% scheduling slack.
                // Clip single spikes, warm up after profile changes, and demand
                // sustained overload: one startup/periodic IDR is not CPU load.
                const double ratio = std::clamp(elapsedMs / periodMs, 0.0, 4.0);
                average = samples == 0 ? std::min(1.0, ratio) : average * .8 + ratio * .2;
                samples = std::min(samples + 1, 1000);
                if (samples < 6 || now - warmSince < 750 || now < cooldownUntil) return;
                if (average <= 1.2) { overloadedSince = -1; return; }
                if (overloadedSince < 0) overloadedSince = now;
                if (now - overloadedSince < 1000) return;
                overloadedSince = -1;
                cooldownUntil = now + 2000;
                if (shared() && it->layerProfiles.contains(QStringLiteral("low"))) {
                    it->lowSuppressed = true;
                    updateProfiles(true);
                } else adaptation.penalize(now);
            });
            QObject::connect(source, &ScreenCaptureSource::layerEncodingFailed, q,
                             [this, i, source](const QString& layer, const QString&) {
                auto it = captures.find(i);
                if (it == captures.end() || it->source != source || layer != QLatin1String("low")) return;
                it->lowSuppressed = true;
                updateProfiles(true);
            });
            QObject::connect(source, &ScreenCaptureSource::errorOccurred, q,
                             [this, i, source](ScreenCaptureError code, const QString& error) {
                auto capture = captures.find(i);
                if (capture == captures.end() || capture->source != source) return;
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
                    capture->lowSuppressed = true;
                    updateProfiles(true);
                    source->setBackpressured(!network->screenSendWindowOpen());
                    source->start(capture->screen);
                    return;
                }
                const auto failedLayers = capture->layerProfiles.keys();
                failedScreens.insert(i);
                source->stop();
                captures.remove(i);
                source->deleteLater();
                setStatus(error);
                const QString reason = code == ScreenCaptureError::PermissionDenied
                    ? QStringLiteral("permission_denied") : QStringLiteral("capture_error");
                qWarning().noquote() << "[ScreenSharing] Screen" << i << reason << error;
                if (shared()) for (const auto& layer : failedLayers)
                    network->sendScreenPublicationStatus(publication.value("publicationId").toString(), i, layer, reason);
                for (auto it = grants.cbegin(); it != grants.cend(); ++it)
                    if (!shared() && requestedEdge(*it, i) > 0)
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
    d->presentationTimer.setInterval(2);
    d->presentationTimer.setTimerType(Qt::PreciseTimer);
    connect(&d->presentationTimer, &QTimer::timeout, this, [this] { d->flushPresentations(); });
    connect(&d->timer, &QTimer::timeout, this, &ScreenSharingService::refresh);
    d->timer.start(AppConfig::instance().screenFeedbackIntervalMs());
    connect(network, &WebSocketClient::screenSourceFeedback, this, [this](int rtt, bool congested) {
        d->adaptation.observe(QStringLiteral("source"), rtt, congested, false, d->clock.elapsed());
    });
    connect(network, &WebSocketClient::screenFrameAdmissionLimited, this,
            [this](const QJsonObject& metadata, qint64 bytes, qint64 maximumBytes) {
        const int screen = metadata.value("screenId").toInt(-1);
        auto capture = d->captures.find(screen);
        if (capture == d->captures.end()) return;
        const auto layer = d->shared() ? metadata.value("layer").toString() : QStringLiteral("main");
        if (!capture->layerProfiles.contains(layer)) return;
        if (d->shared() && metadata.value("publicationId") != d->publication.value("publicationId")) return;
        auto profile = capture->layerProfiles.value(layer);
        profile.maximumEdge = std::min(profile.maximumEdge,
            std::max(metadata.value("width").toInt(), metadata.value("height").toInt()));
        if (capture->admissionCeilings[layer].limit(profile, bytes, maximumBytes, d->clock.elapsed()))
            d->updateProfiles(true);
    });
    connect(network, &WebSocketClient::heartbeatSampleReceived, this,
            [this](quint64, qint64 rtt, qint64, qint64) {
        d->adaptation.observe(QStringLiteral("control"), int(std::min<qint64>(60000, rtt)),
                              false, false, d->clock.elapsed());
    });
    connect(network, &WebSocketClient::screenSendWindowChanged, this, [this] { d->updateBackpressure(); });
    connect(network, &WebSocketClient::screenShareFeedbackReceived, this, [this](const QJsonObject& feedback) {
        if (d->shared()) return; // Each downstream receiver is paced by the relay.
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
        if (!d->shared()) d->failedScreens.clear();
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
        if (!d->shared()) {
            d->adaptation.transportReset(d->clock.elapsed());
            d->grants.clear();
            d->sequences.clear();
            d->stopCaptures();
        }
        d->clearReceived();
        d->subscribedGeneration = 0;
    });
    connect(network, &WebSocketClient::screenPublicationChannelReady, this, [this] {
        d->failedScreens.clear();
        refresh();
    });
    connect(network, &WebSocketClient::screenPublicationChannelUnavailable, this, [this] {
        d->clearPublication();
        d->adaptation.transportReset(d->clock.elapsed());
    });
    connect(network, &WebSocketClient::screenPublicationRequested, this, [this](const QJsonObject& request) {
        if (!d->shared()) return;
        const bool changed = d->publication.value("publicationId") != request.value("publicationId");
        if (changed || !request.value("enabled").toBool()) {
            d->clearPublication();
            d->failedScreens.clear();
        }
        if (request.value("enabled").toBool() && d->enabled && !d->suspended) d->publication = request;
        d->refreshCaptures();
        // Existing encoders absorb changing viewport/layer demand on the
        // regular profile tick; continuous zoom must not reopen them per event.
        // Newly demanded monitors were started immediately by refreshCaptures.
        d->updateProfiles();
    });
    connect(network, &WebSocketClient::screenPublicationKeyFrameRequested, this, [this](const QJsonObject& request) {
        if (!d->publicationReady() || request.value("publicationId") != d->publication.value("publicationId")) return;
        const int screen = request.value("screenId").toInt(-1);
        if (d->captures.contains(screen)) d->captures.value(screen).source->requestKeyFrame(request.value("layer").toString());
    });
    connect(network, &WebSocketClient::screenShareRequestReceived, this, [this](const QJsonObject& request) {
        if (d->shared()) return;
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
        if (d->shared()) d->clearPublication();
        else d->stopCaptures();
        d->failedScreens.clear();
    });
    connect(monitor, &SystemMonitor::screenConfigurationChanged, this, &ScreenSharingService::refresh);
}

ScreenSharingService::~ScreenSharingService()
{
    d->timer.stop();
    d->presentationTimer.stop();
    d->stopCaptures();
    d->decoders.clear();
    d->pool.waitForDone();
}

void ScreenSharingService::setSharingEnabled(bool enabled)
{
    const bool changed = d->enabled != enabled;
    d->enabled = enabled;
    if (changed) d->failedScreens.clear();
    if (!enabled) d->clearPublication();
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
    if (suspended) d->clearPublication();
    d->network->setScreenSharingEnabled(d->enabled && !suspended);
    refresh();
}

void ScreenSharingService::refresh()
{
    d->updateProfiles();
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

void ScreenSharingService::setAudioReservationBps(int bitrate)
{
    bitrate = std::clamp(bitrate, 0, 160000);
    if (d->audioReservation == bitrate) return;
    d->audioReservation = bitrate;
    d->updateProfiles(true);
}

void ScreenSharingService::setAudioPlaybackClock(const QString& endpoint, const QString& epoch, qint64 sourceUs)
{
    if (d->audioEndpoint != endpoint || d->audioEpoch != epoch) {
        d->audioClock.reset();
        d->flushPresentations(true);
    }
    d->audioEndpoint = endpoint;
    d->audioEpoch = epoch;
    d->audioClock.update(sourceUs, d->clock.nsecsElapsed() / 1000);
}

void ScreenSharingService::clearAudioPlaybackClock(const QString& endpoint)
{
    if (d->audioEndpoint != endpoint) return;
    d->audioEndpoint.clear();
    d->audioEpoch.clear();
    d->audioClock.reset();
    d->flushPresentations(true);
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

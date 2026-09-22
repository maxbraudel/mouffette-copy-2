#include "backend/audiosharing/AudioWorkerClient.h"
#include "backend/audiosharing/AudioWorkerProtocol.h"
#include <QCoreApplication>
#include <QDebug>
#include <QLocalServer>
#include <QLoggingCategory>
#include <QProcess>
#include <QSharedMemory>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <new>

namespace {
Q_LOGGING_CATEGORY(audioPlaybackLog, "mouffette.audio.playback")
QCborMap command(const char* type) {
    return {{QStringLiteral("type"), QString::fromLatin1(type)}};
}
QPointer<AudioWorkerClient> singleton;
}
struct AudioWorkerClient::Private {
    QLocalServer server;
    QProcess process;
    QPointer<QLocalSocket> socket;
    QByteArray input;
    QString token, captureEpoch;
    int bitrate = 96000;
    bool capture = false, muted = false, ready = false, closing = false;
    bool unavailableReported = false;
    int restarts = 0;
    qint64 lastPlaybackDiagnosticUs = 0;
    QTimer handshake, previewReplay, previewWatchdog;
    QList<QString> pendingPreviews;
    QHash<QString, qint64> awaitingPreviews;
    QHash<QString, std::weak_ptr<AudioPreviewChannel>> previews;
    void sendCapture() {
        auto message = command(capture ? "capture" : "capture-stop");
        message.insert(QStringLiteral("epoch"), captureEpoch);
        message.insert(QStringLiteral("bitrate"), bitrate);
        AudioWorkerProtocol::sendControl(socket, message);
    }
    void sendPreview(const std::shared_ptr<AudioPreviewChannel>& channel) {
        auto message = command("preview-add");
        message.insert(QStringLiteral("key"), channel->key());
        message.insert(QStringLiteral("device"), channel->deviceId);
        if (AudioWorkerProtocol::sendControl(socket, message)) {
            awaitingPreviews.insert(channel->key(), AudioWorkerClient::nowUs());
            if (!previewWatchdog.isActive()) previewWatchdog.start();
        }
    }
};
AudioWorkerClient* AudioWorkerClient::instance() {
    if (!singleton) singleton = new AudioWorkerClient(QCoreApplication::instance());
    return singleton;
}
AudioWorkerClient::AudioWorkerClient(QObject* parent) : QObject(parent), d(std::make_unique<Private>()) {
    d->server.setSocketOptions(QLocalServer::UserAccessOption);
    d->handshake.setSingleShot(true);
    d->handshake.setInterval(5000);
    d->previewReplay.setSingleShot(true); d->previewReplay.setInterval(5);
    connect(&d->previewReplay, &QTimer::timeout, this, &AudioWorkerClient::replayPreviews);
    d->previewWatchdog.setInterval(1000);
    connect(&d->previewWatchdog, &QTimer::timeout, this, [this] {
        QList<QString> expired;
        const auto now = nowUs();
        for (auto it = d->awaitingPreviews.cbegin(); it != d->awaitingPreviews.cend(); ++it)
            if (now - it.value() >= 5000000) expired.append(it.key());
        for (const auto& key : expired)
            previewAttachment(key, false, QStringLiteral("The audio worker did not acknowledge the preview audio channel"));
    });
    connect(&d->handshake, &QTimer::timeout, this, [this] {
        if (!d->ready && d->process.state() != QProcess::NotRunning) d->process.kill();
    });
    connect(&d->server, &QLocalServer::newConnection, this, [this] {
        while (auto* candidate = d->server.nextPendingConnection()) {
            if (d->socket) { candidate->disconnectFromServer(); candidate->deleteLater(); continue; }
            d->socket = candidate;
            connect(candidate, &QLocalSocket::readyRead, this, &AudioWorkerClient::readMessages);
            connect(candidate, &QLocalSocket::disconnected, this, [this, candidate] {
                if (d->socket == candidate) {
                    d->socket = nullptr; d->ready = false; d->input.clear();
                    d->previewReplay.stop(); d->pendingPreviews.clear();
                    d->previewWatchdog.stop(); d->awaitingPreviews.clear();
                    // Critical IPC failure is fail-closed even if the helper's
                    // GUI thread cannot promptly process socket disconnection.
                    if (!d->closing && d->process.state() != QProcess::NotRunning) d->process.kill();
                }
                candidate->deleteLater();
            });
        }
    });
    connect(&d->process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (d->closing || error != QProcess::FailedToStart) return;
        d->handshake.stop(); d->ready = false;
        ++d->restarts;
        const auto message = QStringLiteral("The audio worker could not start: %1").arg(d->process.errorString());
        emit captureStateChanged(false, message); emit failed(message);
    });
    connect(&d->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int exitCode, QProcess::ExitStatus exitStatus) {
            d->handshake.stop(); d->ready = false;
            d->previewWatchdog.stop(); d->awaitingPreviews.clear();
            for (const auto& weak : d->previews) if (const auto preview = weak.lock()) {
                preview->workerAttached = false;
                preview->state()->consumerAvailable.store(false);
            }
            if (auto* socket = d->socket.data()) {
                d->socket = nullptr;
                socket->abort(); socket->deleteLater();
            }
            d->input.clear();
            if (d->closing) return;
            const auto failure = QStringLiteral("The audio worker stopped (%1, exit code 0x%2)")
                .arg(exitStatus == QProcess::CrashExit ? QStringLiteral("crash") : QStringLiteral("normal exit"))
                .arg(quint32(exitCode), 8, 16, QLatin1Char('0'));
            qWarning().noquote() << "[AudioWorker]" << failure;
            emit captureStateChanged(false, failure);
            // Never leak excluded preview audio into the main process as a fallback.
            if (++d->restarts <= 3 && (d->capture || !d->previews.isEmpty()))
                QTimer::singleShot(250 * d->restarts, this, [this] { ensureWorker(); });
            else emit failed(QStringLiteral("The audio worker stopped; audio is unavailable"));
        });
}
AudioWorkerClient::~AudioWorkerClient() { shutdown(); }
void AudioWorkerClient::ensureWorker() {
    if (d->closing || d->restarts > 3 || d->process.state() != QProcess::NotRunning) return;
    const auto executable = QCoreApplication::instance()->property("mouffetteAudioWorkerExecutable").toString();
    // Test binaries do not implement --audio-worker. They can supply an actual
    // worker executable explicitly, and otherwise keep preparation headless.
    if (executable.isEmpty()) {
        if (!d->unavailableReported) {
            d->unavailableReported = true;
            emit captureStateChanged(false, QStringLiteral("Audio worker executable is unavailable"));
        }
        return;
    }
    d->server.close();
    const auto name = QStringLiteral("mouffette-audio-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    d->token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!d->server.listen(name)) { emit failed(d->server.errorString()); return; }
    d->process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    d->process.start(executable, {QStringLiteral("--audio-worker"), name, d->token});
    d->handshake.start();
}
void AudioWorkerClient::readMessages() {
    if (!d->socket) return;
    d->input += d->socket->readAll();
    if (d->input.size() > 2 * AudioWorkerProtocol::MaximumMessage) { d->socket->abort(); return; }
    QCborMap message;
    bool malformed = false;
    while (d->socket && AudioWorkerProtocol::take(d->input, message, malformed)) {
        const auto type = message.value(QStringLiteral("type")).toString();
        if (!d->ready) {
            if (type != QLatin1String("hello") || message.value(QStringLiteral("token")).toString() != d->token) {
                d->socket->abort(); return;
            }
            d->ready = true; d->handshake.stop();
            const auto pid = d->process.processId();
            QTimer::singleShot(30000, this, [this, pid] {
                if (d->ready && d->process.processId() == pid) d->restarts = 0;
            });
            auto muted = command("mute"); muted.insert(QStringLiteral("muted"), d->muted);
            AudioWorkerProtocol::sendControl(d->socket, muted);
            d->sendCapture();
            d->pendingPreviews.clear();
            for (auto it = d->previews.begin(); it != d->previews.end();) {
                if (!it.value().expired()) { d->pendingPreviews.append(it.key()); ++it; }
                else it = d->previews.erase(it);
            }
            replayPreviews();
        } else if (type == QLatin1String("preview-state")) {
            previewAttachment(message.value(QStringLiteral("key")).toString(),
                message.value(QStringLiteral("attached")).toBool(), message.value(QStringLiteral("error")).toString());
        } else if (type == QLatin1String("packet")) {
            if (!d->capture || message.value(QStringLiteral("epoch")).toString() != d->captureEpoch) continue;
            const auto timestamp = message.value(QStringLiteral("timestamp")).toInteger(-1);
            const auto sequence = message.value(QStringLiteral("sequence")).toInteger(-1);
            const auto packet = message.value(QStringLiteral("opus")).toByteArray();
            const auto age = nowUs() - timestamp;
            // A blocked UI can leave old packets inside the local socket's
            // kernel buffer even when bytesToWrite() was zero in the helper.
            if (timestamp < 0 || sequence < 0 || packet.isEmpty() || packet.size() > 1275
                || age < -250000 || age > 150000) continue;
            emit packetReady(d->captureEpoch, quint64(sequence), timestamp, packet);
        } else if (type == QLatin1String("capture-state")) {
            const auto epoch = message.value(QStringLiteral("epoch")).toString();
            const bool active = message.value(QStringLiteral("active")).toBool();
            const auto failure = message.value(QStringLiteral("error")).toString();
            if (epoch == d->captureEpoch && (d->capture || (!active && failure.isEmpty())))
                emit captureStateChanged(active, failure);
        } else if (type == QLatin1String("clock")) {
            const auto localUs = message.value(QStringLiteral("localUs")).toInteger(-1);
            const auto timestampUs = message.value(QStringLiteral("timestamp")).toInteger(-1);
            const auto ageUs = nowUs() - localUs;
            // The sample/device anchor belongs to the worker, not this queued
            // IPC callback. A stalled GUI must never turn stale telemetry into
            // an apparently fresh audio/video synchronization point.
            if (d->muted || localUs < 0 || timestampUs < 0 || ageUs < -100000 || ageUs > 100000) continue;
            emit playbackClock(message.value(QStringLiteral("source")).toString(),
                message.value(QStringLiteral("epoch")).toString(), timestampUs, localUs);
        } else if (type == QLatin1String("feedback")) {
            const auto concealed = message.value(QStringLiteral("concealed")).toInteger();
            const auto underruns = message.value(QStringLiteral("underruns")).toInteger();
            const auto rebuffers = message.value(QStringLiteral("rebuffers")).toInteger();
            const auto diagnosticUs = nowUs();
            if ((concealed > 0 || underruns > 0 || rebuffers > 0)
                && diagnosticUs - d->lastPlaybackDiagnosticUs >= 5000000) {
                d->lastPlaybackDiagnosticUs = diagnosticUs;
                qCInfo(audioPlaybackLog) << "Playback recovery in latest feedback interval:"
                    << "concealed" << concealed << "underruns" << underruns
                    << "rebuffers" << rebuffers << "bufferedMs"
                    << message.value(QStringLiteral("buffered")).toInteger();
            }
            emit playbackFeedback(message.value(QStringLiteral("source")).toString(),
                message.value(QStringLiteral("epoch")).toString(), int(message.value(QStringLiteral("dropped")).toInteger()),
                int(message.value(QStringLiteral("buffered")).toInteger()));
        } else if (type == QLatin1String("error")) emit playbackFailed(message.value(QStringLiteral("error")).toString());
    }
    if (malformed && d->socket) d->socket->abort();
}
void AudioWorkerClient::previewAttachment(const QString& key, bool attached, const QString& error) {
    // Ignore late replies for deleted channels, expired requests or old workers.
    if (!d->awaitingPreviews.remove(key)) return;
    if (d->awaitingPreviews.isEmpty()) d->previewWatchdog.stop();
    const auto channel = d->previews.value(key).lock();
    if (!channel) return;
    channel->workerAttached = attached;
    const auto failure = attached ? QString() : error.isEmpty()
        ? QStringLiteral("The audio worker rejected the preview audio channel") : error;
    if (!attached) {
        channel->state()->consumerAvailable.store(false, std::memory_order_release);
        qCWarning(audioPlaybackLog).noquote() << "Preview audio channel" << key << "failed:" << failure;
    }
    // A local preview failure must never reset otherwise healthy remote audio.
    emit previewAttachmentChanged(key, attached, failure);
}
void AudioWorkerClient::replayPreviews() {
    if (!d->ready || !d->socket) return;
    int sent = 0;
    while (!d->pendingPreviews.isEmpty() && d->socket
           && d->socket->bytesToWrite() < AudioWorkerProtocol::MaximumBacklog / 2 && sent++ < 8) {
        const auto key = d->pendingPreviews.takeFirst();
        if (const auto channel = d->previews.value(key).lock()) d->sendPreview(channel);
    }
    if (d->ready && !d->pendingPreviews.isEmpty()) d->previewReplay.start();
}
void AudioWorkerClient::startCapture(const QString& epoch, int bitrateBps) {
    if (epoch.isEmpty()) return;
    if (d->captureEpoch != epoch) d->restarts = 0;
    d->capture = true; d->captureEpoch = epoch; d->bitrate = bitrateBps <= 32000 ? 32000 : 96000;
    ensureWorker(); if (d->ready) d->sendCapture();
}
void AudioWorkerClient::setCaptureBitrate(int bitrateBps) {
    const int bitrate = bitrateBps <= 32000 ? 32000 : 96000;
    if (d->bitrate == bitrate) return;
    d->bitrate = bitrate;
    if (d->ready) {
        auto message = command("bitrate"); message.insert(QStringLiteral("bitrate"), d->bitrate);
        AudioWorkerProtocol::sendControl(d->socket, message);
    }
}
void AudioWorkerClient::stopCapture() { d->capture = false; if (d->ready) d->sendCapture(); }
void AudioWorkerClient::playPacket(const QString& source, const QString& epoch, quint64 sequence,
                                  qint64 timestampUs, const QByteArray& opus, qint64 presentationUs) {
    if (d->muted || source.isEmpty() || epoch.isEmpty() || opus.isEmpty() || opus.size() > 1275 || timestampUs < 0) return;
    ensureWorker(); if (!d->ready) return; // A live stream never accumulates startup packets.
    auto message = command("play");
    message.insert(QStringLiteral("source"), source); message.insert(QStringLiteral("epoch"), epoch);
    message.insert(QStringLiteral("sequence"), qint64(sequence)); message.insert(QStringLiteral("timestamp"), timestampUs);
    message.insert(QStringLiteral("receivedAt"), nowUs());
    message.insert(QStringLiteral("presentation"), presentationUs);
    message.insert(QStringLiteral("opus"), opus); AudioWorkerProtocol::send(d->socket, message);
}
void AudioWorkerClient::resetPlayback(const QString& source) {
    if (!d->ready) return;
    auto message = command("reset"); message.insert(QStringLiteral("source"), source);
    AudioWorkerProtocol::sendControl(d->socket, message);
}
void AudioWorkerClient::setPlaybackMuted(bool muted) {
    if (d->muted && !muted) d->restarts = 0;
    d->muted = muted;
    if (d->ready) { auto message = command("mute"); message.insert(QStringLiteral("muted"), muted); AudioWorkerProtocol::sendControl(d->socket, message); }
}
std::shared_ptr<AudioPreviewChannel> AudioWorkerClient::createPreviewChannel(const QByteArray& deviceId) {
    if (d->previews.size() >= 256) {
        qCWarning(audioPlaybackLog) << "Too many preview audio channels"; return {};
    }
    auto channel = std::make_shared<AudioPreviewChannel>();
    channel->owner = this;
    channel->deviceId = deviceId;
    channel->memory = std::make_unique<QSharedMemory>(QStringLiteral("mouffette-pcm-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!channel->memory->create(sizeof(AudioPreviewState))) {
        qCWarning(audioPlaybackLog) << "Could not allocate preview audio shared memory:" << channel->memory->errorString();
        return {};
    }
    new (channel->memory->data()) AudioPreviewState;
    d->previews.insert(channel->key(), channel);
    ensureWorker();
    if (d->ready) { d->pendingPreviews.append(channel->key()); replayPreviews(); }
    return channel;
}
void AudioWorkerClient::removePreview(const QString& key) {
    d->previews.remove(key);
    d->pendingPreviews.removeAll(key); d->awaitingPreviews.remove(key);
    if (d->awaitingPreviews.isEmpty()) d->previewWatchdog.stop();
    if (d->ready) { auto message = command("preview-remove"); message.insert(QStringLiteral("key"), key); AudioWorkerProtocol::sendControl(d->socket, message); }
}
void AudioWorkerClient::shutdown() {
    if (!d || d->closing) return;
    d->closing = true; d->handshake.stop();
    d->previewReplay.stop(); d->previewWatchdog.stop();
    d->pendingPreviews.clear(); d->awaitingPreviews.clear();
    if (const QPointer<QLocalSocket> socket = d->socket) {
        AudioWorkerProtocol::sendControl(socket, command("quit"));
        if (socket) socket->flush();
    }
    if (d->process.state() != QProcess::NotRunning && !d->process.waitForFinished(500)) {
        d->process.kill(); d->process.waitForFinished(1000);
    }
    d->server.close();
}
AudioPreviewChannel::~AudioPreviewChannel() {
    if (state()) state()->playing.store(false, std::memory_order_release);
    if (owner) {
        const auto keyValue = key(); const QPointer<AudioWorkerClient> client = owner;
        if (QThread::currentThread() == client->thread()) client->removePreview(keyValue);
        else QMetaObject::invokeMethod(client, [client, keyValue] { if (client) client->removePreview(keyValue); }, Qt::QueuedConnection);
    }
}
AudioPreviewState* AudioPreviewChannel::state() const {
    return memory && memory->isAttached() ? static_cast<AudioPreviewState*>(memory->data()) : nullptr;
}
QString AudioPreviewChannel::key() const { return memory ? memory->key() : QString(); }

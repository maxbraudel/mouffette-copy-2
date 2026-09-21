#include "backend/audiosharing/AudioWorkerClient.h"
#include "backend/audiosharing/AudioWorkerProtocol.h"
#include <QCoreApplication>
#include <QLocalServer>
#include <QProcess>
#include <QSharedMemory>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <new>

namespace {
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
    QTimer handshake, previewReplay;
    QList<QString> pendingPreviews;
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
        AudioWorkerProtocol::sendControl(socket, message);
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
        [this](int, QProcess::ExitStatus) {
            d->handshake.stop(); d->ready = false;
            for (const auto& weak : d->previews) if (const auto preview = weak.lock())
                preview->state()->consumerAvailable.store(false);
            if (auto* socket = d->socket.data()) {
                d->socket = nullptr;
                socket->abort(); socket->deleteLater();
            }
            d->input.clear();
            if (d->closing) return;
            emit captureStateChanged(false, QStringLiteral("The audio worker stopped"));
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
        } else if (type == QLatin1String("packet")) {
            if (!d->capture || message.value(QStringLiteral("epoch")).toString() != d->captureEpoch) continue;
            emit packetReady(d->captureEpoch, quint64(message.value(QStringLiteral("sequence")).toInteger()),
                message.value(QStringLiteral("timestamp")).toInteger(), message.value(QStringLiteral("opus")).toByteArray());
        } else if (type == QLatin1String("capture-state")) {
            const auto epoch = message.value(QStringLiteral("epoch")).toString();
            const bool active = message.value(QStringLiteral("active")).toBool();
            const auto failure = message.value(QStringLiteral("error")).toString();
            if (epoch == d->captureEpoch && (d->capture || (!active && failure.isEmpty())))
                emit captureStateChanged(active, failure);
        } else if (type == QLatin1String("clock")) {
            emit playbackClock(message.value(QStringLiteral("source")).toString(),
                message.value(QStringLiteral("epoch")).toString(), message.value(QStringLiteral("timestamp")).toInteger());
        } else if (type == QLatin1String("feedback")) {
            emit playbackFeedback(message.value(QStringLiteral("source")).toString(),
                message.value(QStringLiteral("epoch")).toString(), int(message.value(QStringLiteral("dropped")).toInteger()),
                int(message.value(QStringLiteral("buffered")).toInteger()));
        } else if (type == QLatin1String("error")) emit playbackFailed(message.value(QStringLiteral("error")).toString());
    }
    if (malformed && d->socket) d->socket->abort();
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
                                  qint64 timestampUs, const QByteArray& opus) {
    if (d->muted || source.isEmpty() || epoch.isEmpty() || opus.isEmpty() || opus.size() > 1275 || timestampUs < 0) return;
    ensureWorker(); if (!d->ready) return; // A live stream never accumulates startup packets.
    auto message = command("play");
    message.insert(QStringLiteral("source"), source); message.insert(QStringLiteral("epoch"), epoch);
    message.insert(QStringLiteral("sequence"), qint64(sequence)); message.insert(QStringLiteral("timestamp"), timestampUs);
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
    if (d->previews.size() >= 256) return {};
    auto channel = std::make_shared<AudioPreviewChannel>();
    channel->owner = this;
    channel->deviceId = deviceId;
    channel->memory = std::make_unique<QSharedMemory>(QStringLiteral("mouffette-pcm-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!channel->memory->create(sizeof(AudioPreviewState))) return {};
    new (channel->memory->data()) AudioPreviewState;
    d->previews.insert(channel->key(), channel);
    ensureWorker();
    if (d->ready) { d->pendingPreviews.append(channel->key()); replayPreviews(); }
    return channel;
}
void AudioWorkerClient::removePreview(const QString& key) {
    d->previews.remove(key);
    if (d->ready) { auto message = command("preview-remove"); message.insert(QStringLiteral("key"), key); AudioWorkerProtocol::sendControl(d->socket, message); }
}
void AudioWorkerClient::shutdown() {
    if (!d || d->closing) return;
    d->closing = true; d->handshake.stop();
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

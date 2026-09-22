#include "AudioSharingService.h"
#include "AudioEngine.h"
#include "MediaCaptureClock.h"
#include "backend/network/AudioTransport.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include <QDebug>

AudioSharingService::AudioSharingService(WebSocketClient* network, QObject* parent, AudioEngine* audio)
    : QObject(parent), m_network(network), m_transport(new AudioTransport(network, this)),
      m_audio(audio ? audio : AudioEngine::instance())
{
    connect(m_transport, &AudioTransport::sourceReservationChanged,
            this, &AudioSharingService::sourceReservationChanged);
    connect(m_transport, &AudioTransport::publicationChanged, this, [this](bool, int) { refresh(); });
    connect(m_transport, &AudioTransport::playbackStreamChanged, this, [this](const QString& stream) {
        m_playbackAllowed = false;
        clearPlayback();
        m_stream = stream;
        if (!stream.isEmpty() && !m_endpoint.isEmpty() && m_listeningEnabled) {
            m_hadPlaybackStream = true;
            m_waitingForChannel = false;
            setState(QStringLiteral("loading"), tr("Connecting to remote system audio…"));
        }
    });
    connect(m_transport, &AudioTransport::packetReceived, this,
            [this](const QByteArray& opus, qint64 timestampUs, quint64 sequence) {
        if (m_stopped || m_suspended || !m_listeningEnabled || !m_playbackAllowed || m_session.isEmpty()
            || m_endpoint.isEmpty() || m_stream.isEmpty()) return;
        m_audio->playPacket(m_endpoint, m_stream, sequence, timestampUs, opus,
                             m_transport->playbackTimeUs(timestampUs));
    });
    connect(m_transport, &AudioTransport::remoteStateChanged, this, [this](const QString& reason) {
        if (!m_listeningEnabled || m_endpoint.isEmpty() || m_suspended || m_stopped) return;
        m_waitingForChannel = false;
        if (reason == QLatin1String("streaming") || reason == QLatin1String("starting")) {
            m_playbackAllowed = true;
            // A ready native capture can legitimately produce no packets while
            // the system is silent. Only playbackClock supplies A/V timing.
            if (reason == QLatin1String("streaming"))
                setState(QStringLiteral("available"), tr("Remote system audio sharing is active."));
            else setState(QStringLiteral("loading"), tr("Waiting for remote system audio…"));
        } else {
            m_playbackAllowed = false;
            clearPlayback();
            // The publisher opens its independent audio socket asynchronously.
            // A first subscription can arrive before that ordinary handshake.
            if (reason == QLatin1String("channel_unavailable") && !m_hadPlaybackStream
                && MediaCaptureClock::nowUs() - m_subscriptionStartedAt < 15000000) {
                m_waitingForChannel = true;
                setState(QStringLiteral("loading"), tr("Connecting to remote system audio…"));
                return;
            }
            QString state = QStringLiteral("unavailable");
            QString message = tr("System audio is unavailable on the remote client.");
            if (reason == QLatin1String("disabled") || reason == QLatin1String("consent_disabled")) {
                state = QStringLiteral("sharing_disabled");
                message = tr("The remote user has disabled system audio sharing.");
            } else if (reason == QLatin1String("capture_error") || reason == QLatin1String("permission_denied")) {
                state = QStringLiteral("error");
                message = reason == QLatin1String("permission_denied")
                    ? tr("The remote system denied permission to capture system audio.")
                    : tr("Remote system audio could not be captured or encoded. Check the sharing client's Settings and system permissions.");
            } else if (reason == QLatin1String("channel_unavailable") || reason == QLatin1String("timing_unavailable")) {
                state = QStringLiteral("error");
                message = reason == QLatin1String("timing_unavailable")
                    ? tr("Remote audio exceeded the synchronization delay limit. Reconnecting…")
                    : tr("The remote system audio connection is unavailable. Reconnecting…");
            } else if (reason == QLatin1String("unsupported")) {
                message = tr("The remote client or server does not support system audio sharing.");
            } else if (reason == QLatin1String("capacity_limited")) {
                message = tr("System audio is unavailable because the remote client's sharing limit has been reached.");
            } else if (reason == QLatin1String("session_unavailable")) {
                message = tr("The remote session is unavailable.");
            }
            setState(state, message);
            if (state == QLatin1String("error") || state == QLatin1String("sharing_disabled")
                || reason == QLatin1String("capacity_limited") || reason == QLatin1String("unavailable")) report(message);
        }
    });
    connect(m_transport, &AudioTransport::publicationIssue, this, [this](const QString& message) {
        if (m_enabled && !m_suspended && !m_stopped && m_status != message) {
            m_captureHasError = true;
            m_status = message;
            emit statusChanged();
        }
    });
    connect(m_audio, &AudioEngine::packetReady, this,
            [this](const QString& epoch, quint64 sequence, qint64 timestampUs, const QByteArray& opus) {
        if (m_enabled && !m_suspended && !m_stopped && epoch == m_captureEpoch
            && epoch == m_transport->publicationId()) m_transport->sendPacket(opus, timestampUs, sequence);
    });
    connect(m_audio, &AudioEngine::captureStateChanged, this,
            [this](bool active, const QString& error) {
        if (m_stopped || m_captureEpoch.isEmpty()) return;
        const QString value = active ? tr("Sharing system audio.")
            : !error.isEmpty() ? error : m_enabled ? m_status : QString();
        if (m_status != value) { m_status = value; emit statusChanged(); }
        if (!error.isEmpty()) captureFailed(error);
        else if (active) {
            m_captureHasError = false;
            m_transport->sendStatus(QStringLiteral("streaming"));
        }
    });
    connect(m_audio, &AudioEngine::playbackClock, this,
            [this](const QString& source, const QString& epoch, qint64 sourceUs, qint64 localUs) {
        if (m_stopped || !m_listeningEnabled || m_suspended || !m_playbackAllowed || source != m_endpoint
            || epoch != m_stream || m_session.isEmpty()) return;
        setState(QStringLiteral("available"), tr("Receiving remote system audio."));
        m_reported.clear();
        emit playbackClock(source, epoch, sourceUs, localUs);
    });
    connect(m_audio, &AudioEngine::playbackFeedback, this,
            [this](const QString& source, const QString& epoch, int dropped, int bufferedMs) {
        if (!m_stopped && m_listeningEnabled && !m_suspended && m_playbackAllowed && source == m_endpoint && epoch == m_stream)
            m_transport->sendPlaybackFeedback(dropped, bufferedMs);
    });
    connect(m_audio, &AudioEngine::playbackFailed, this,
            [this](const QString& source, const QString& epoch, const QString& error) {
        if (m_stopped || m_suspended || !m_listeningEnabled || source != m_endpoint
            || epoch != m_stream || !m_playbackAllowed) return;
        clearPlayback();
        setState(QStringLiteral("error"), error);
        report(error);
    });
    connect(network, &WebSocketClient::connected, this, &AudioSharingService::refresh);
    connect(network, &WebSocketClient::disconnected, this, &AudioSharingService::refresh);
    connect(network->remoteSessionCoordinator(), &RemoteSessionCoordinator::sessionChanged,
            this, &AudioSharingService::refresh);
    connect(network->remoteSessionCoordinator(), &RemoteSessionCoordinator::sessionRemoved,
            this, &AudioSharingService::refresh);
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &AudioSharingService::refresh);
    m_timer.start();
}

AudioSharingService::~AudioSharingService() { stop(); }

void AudioSharingService::setState(const QString& value, const QString& message)
{
    const bool changed = m_state != value;
    m_state = value;
    if (m_remoteStatus != message) { m_remoteStatus = message; emit remoteStatusChanged(); }
    if (changed) emit stateChanged();
}

void AudioSharingService::captureFailed(const QString& message)
{
    qWarning().noquote() << "[AudioSharing] System audio capture failed:" << message;
    if (m_status != message) { m_status = message; emit statusChanged(); }
    m_transport->sendStatus(QStringLiteral("capture_error"));
    m_captureEpoch.clear();
    m_captureRetryAt = MediaCaptureClock::nowUs() + 3000000;
    m_captureNeedsNewEpoch = true;
    m_captureHasError = true;
    m_audio->stopCapture();
    // Keep the publication's failure visible during backoff. Before restarting
    // the codec, rotate the wire epoch so old sequence state cannot survive.
}

void AudioSharingService::report(const QString& message)
{
    if (message.isEmpty() || m_endpoint.isEmpty() || !m_listeningEnabled || m_suspended || m_stopped
        || m_reported.contains(message)) return;
    m_reported.insert(message);
    emit remoteIssue(m_endpoint, message);
}

void AudioSharingService::clearPlayback()
{
    if (!m_endpoint.isEmpty()) {
        m_audio->resetPlayback(m_endpoint);
        emit playbackReset(m_endpoint);
    }
}

void AudioSharingService::setViewedEndpoint(const QString& endpoint)
{
    if (m_endpoint == endpoint || m_stopped) return;
    m_playbackAllowed = false;
    clearPlayback();
    if (!m_session.isEmpty()) m_transport->setSubscription(m_session, m_generation, false);
    m_session.clear(); m_generation = 0; m_stream.clear();
    m_endpoint = endpoint;
    m_reported.clear();
    refresh();
}

void AudioSharingService::setListeningEnabled(bool enabled)
{
    if (m_listeningEnabled == enabled) return;
    m_listeningEnabled = enabled;
    m_audio->setPlaybackMuted(!enabled);
    if (!enabled) clearPlayback();
    m_reported.clear();
    refresh();
}

void AudioSharingService::setSharingEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled) {
        m_audio->stopCapture();
        m_captureEpoch.clear();
        m_captureRetryAt = 0;
        m_captureNeedsNewEpoch = false;
        m_captureHasError = false;
        if (!m_status.isEmpty()) { m_status.clear(); emit statusChanged(); }
    }
    m_transport->setSharingEnabled(enabled);
    refresh();
}

void AudioSharingService::setSuspended(bool suspended)
{
    if (m_suspended == suspended) return;
    m_suspended = suspended;
    if (suspended) {
        m_audio->stopCapture(); m_captureEpoch.clear();
        m_captureNeedsNewEpoch = false; m_captureRetryAt = 0;
    }
    m_transport->setSuspended(suspended);
    refresh();
}

void AudioSharingService::setSourceBudget(int totalBps) { m_transport->setSourceBudget(totalBps); }

void AudioSharingService::observeVideoTimestamp(const QString& endpoint, qint64 sourceUs, qint64 receivedAtUs)
{
    if (!m_stopped && endpoint == m_endpoint && !m_session.isEmpty())
        m_transport->observeVideoTimestamp(sourceUs, receivedAtUs);
}

void AudioSharingService::refresh()
{
    if (m_stopped) return;
    const auto binding = m_network->remoteSessionCoordinator()->outgoingForPeer(m_endpoint);
    const bool listen = m_listeningEnabled && !m_suspended && !m_endpoint.isEmpty()
        && m_network->isConnected() && m_transport->isSupported() && binding.active
        && m_network->canIssueSessionCommands(binding.remoteSessionId);
    if (!listen) {
        m_playbackAllowed = false;
        m_waitingForChannel = false;
        if (!m_session.isEmpty()) {
            m_transport->setSubscription(m_session, m_generation, false);
            clearPlayback();
            m_session.clear(); m_generation = 0; m_stream.clear();
        }
        setState(!m_listeningEnabled ? QStringLiteral("disabled") : QStringLiteral("unavailable"),
            !m_listeningEnabled ? tr("Remote system audio is disabled on this device.")
            : m_suspended ? tr("System audio is suspended while this device is inactive.")
            : m_network->isConnected() && !m_transport->isSupported()
                ? tr("The server does not support system audio sharing.")
                : tr("The remote session is unavailable."));
    } else if (m_session != binding.remoteSessionId || m_generation != binding.generation) {
        m_playbackAllowed = false;
        clearPlayback();
        m_session = binding.remoteSessionId;
        m_generation = binding.generation;
        m_stream.clear();
        m_hadPlaybackStream = false;
        m_waitingForChannel = false;
        m_subscriptionStartedAt = MediaCaptureClock::nowUs();
        m_audio->setPlaybackMuted(false);
        setState(QStringLiteral("loading"), tr("Connecting to remote system audio…"));
        m_transport->setSubscription(m_session, m_generation, true);
    }
    const qint64 now = MediaCaptureClock::nowUs();
    if (listen && m_waitingForChannel && now - m_subscriptionStartedAt >= 15000000) {
        m_waitingForChannel = false;
        const auto message = tr("The remote system audio connection is unavailable. Reconnecting…");
        setState(QStringLiteral("error"), message);
        report(message);
    }
    const QString epoch = m_transport->publicationId();
    if (m_enabled && !m_suspended && m_transport->isPublishing() && !epoch.isEmpty()) {
        if (m_captureNeedsNewEpoch && now >= m_captureRetryAt) {
            m_captureNeedsNewEpoch = false;
            m_transport->restartPublication();
            return;
        }
        if (m_captureEpoch != epoch && now >= m_captureRetryAt) {
            m_captureEpoch = epoch;
            m_captureHasError = false;
            const auto message = tr("Starting system audio sharing…");
            if (m_status != message) { m_status = message; emit statusChanged(); }
            m_audio->startCapture(epoch, m_transport->audioBitrateBps());
        } else if (m_captureEpoch == epoch) m_audio->setCaptureBitrate(m_transport->audioBitrateBps());
    } else {
        if (!m_captureEpoch.isEmpty()) {
            m_captureEpoch.clear();
            m_audio->stopCapture();
        }
        if (!m_status.isEmpty() && (!m_enabled || m_suspended || !m_captureHasError)) {
            m_status.clear(); emit statusChanged();
        }
    }
}

void AudioSharingService::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    m_playbackAllowed = false;
    m_timer.stop();
    clearPlayback();
    m_audio->stopCapture();
    m_transport->stop();
    m_session.clear(); m_stream.clear(); m_captureEpoch.clear();
    setState(QStringLiteral("unavailable"));
}

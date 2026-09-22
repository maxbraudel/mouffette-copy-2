#include "AudioSharingService.h"
#include "AudioWorkerClient.h"
#include "MediaCaptureClock.h"
#include "backend/network/AudioTransport.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include <QDebug>

AudioSharingService::AudioSharingService(WebSocketClient* network, QObject* parent)
    : QObject(parent), m_network(network), m_transport(new AudioTransport(network, this)),
      m_worker(AudioWorkerClient::instance())
{
    connect(m_transport, &AudioTransport::sourceReservationChanged,
            this, &AudioSharingService::sourceReservationChanged);
    connect(m_transport, &AudioTransport::publicationChanged, this, [this](bool, int) { refresh(); });
    connect(m_transport, &AudioTransport::playbackStreamChanged, this, [this](const QString& stream) {
        m_playbackAllowed = false;
        clearPlayback();
        m_stream = stream;
        if (!stream.isEmpty() && !m_endpoint.isEmpty() && !m_muted) setState(QStringLiteral("loading"));
    });
    connect(m_transport, &AudioTransport::packetReceived, this,
            [this](const QByteArray& opus, qint64 timestampUs, quint64 sequence) {
        if (m_stopped || m_suspended || m_muted || !m_playbackAllowed || m_session.isEmpty()
            || m_endpoint.isEmpty() || m_stream.isEmpty()) return;
        m_worker->playPacket(m_endpoint, m_stream, sequence, timestampUs, opus,
                             m_transport->playbackTimeUs(timestampUs));
    });
    connect(m_transport, &AudioTransport::remoteStateChanged, this, [this](const QString& reason) {
        if (m_muted || m_endpoint.isEmpty() || m_suspended || m_stopped) return;
        if (reason == QLatin1String("streaming") || reason == QLatin1String("starting")) {
            m_playbackAllowed = true;
            if (m_state != QLatin1String("available")) setState(QStringLiteral("loading"));
        } else {
            m_playbackAllowed = false;
            clearPlayback();
            setState(QStringLiteral("unavailable"));
            if (reason == QLatin1String("disabled") || reason == QLatin1String("consent_disabled"))
                report(tr("Screen and system audio sharing is disabled on the remote client."));
            else if (reason == QLatin1String("capture_error") || reason == QLatin1String("permission_denied"))
                report(tr("Remote system audio could not be captured. Check the sharing client's Settings and system permissions."));
        }
    });
    connect(m_transport, &AudioTransport::issue, this, &AudioSharingService::report);
    connect(m_worker, &AudioWorkerClient::packetReady, this,
            [this](const QString& epoch, quint64, qint64 timestampUs, const QByteArray& opus) {
        if (m_enabled && !m_suspended && !m_stopped && epoch == m_captureEpoch
            && epoch == m_transport->publicationId()) m_transport->sendPacket(opus, timestampUs);
    });
    connect(m_worker, &AudioWorkerClient::captureStateChanged, this,
            [this](bool active, const QString& error) {
        if (m_stopped) return;
        const QString value = active ? tr("Sharing system audio.")
            : !error.isEmpty() ? error : m_enabled ? m_status : QString();
        if (m_status != value) { m_status = value; emit statusChanged(); }
        if (!error.isEmpty() && !m_captureEpoch.isEmpty()) {
            qWarning().noquote() << "[AudioSharing] System audio capture failed:" << error;
            m_transport->sendStatus(QStringLiteral("capture_error"));
            m_captureEpoch.clear();
            m_captureRetryAt = MediaCaptureClock::nowUs() + 3000000;
            m_worker->stopCapture();
        } else if (active) m_transport->sendStatus(QStringLiteral("streaming"));
    });
    connect(m_worker, &AudioWorkerClient::playbackClock, this,
            [this](const QString& source, const QString& epoch, qint64 sourceUs) {
        if (m_stopped || m_muted || m_suspended || !m_playbackAllowed || source != m_endpoint
            || epoch != m_stream || m_session.isEmpty()) return;
        setState(QStringLiteral("available"));
        m_reported.clear();
        emit playbackClock(source, epoch, sourceUs);
    });
    connect(m_worker, &AudioWorkerClient::playbackFeedback, this,
            [this](const QString& source, const QString& epoch, int dropped, int bufferedMs) {
        if (!m_stopped && !m_muted && !m_suspended && m_playbackAllowed && source == m_endpoint && epoch == m_stream)
            m_transport->sendPlaybackFeedback(dropped, bufferedMs);
    });
    connect(m_worker, &AudioWorkerClient::failed, this, [this](const QString& error) {
        qWarning().noquote() << "[AudioSharing] Audio worker failed:" << error;
        clearPlayback();
        m_captureEpoch.clear();
        m_captureRetryAt = MediaCaptureClock::nowUs() + 3000000;
        if (m_status != error) { m_status = error; emit statusChanged(); }
        if (!m_endpoint.isEmpty() && !m_muted) { setState(QStringLiteral("unavailable")); report(error); }
    });
    connect(m_worker, &AudioWorkerClient::playbackFailed, this, [this](const QString& error) {
        if (m_stopped) return;
        clearPlayback();
        if (!m_endpoint.isEmpty() && !m_muted) {
            setState(QStringLiteral("unavailable"));
            report(error);
        }
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

void AudioSharingService::setState(const QString& value)
{
    if (m_state == value) return;
    m_state = value;
    emit stateChanged();
}

void AudioSharingService::report(const QString& message)
{
    if (message.isEmpty() || m_endpoint.isEmpty() || m_muted || m_suspended || m_stopped
        || m_reported.contains(message)) return;
    m_reported.insert(message);
    emit remoteIssue(m_endpoint, message);
}

void AudioSharingService::clearPlayback()
{
    if (!m_endpoint.isEmpty()) {
        m_worker->resetPlayback(m_endpoint);
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

void AudioSharingService::setMuted(bool muted)
{
    if (m_muted == muted) return;
    m_muted = muted;
    m_worker->setPlaybackMuted(muted);
    if (muted) clearPlayback();
    m_reported.clear();
    refresh();
}

void AudioSharingService::setSharingEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled) {
        m_worker->stopCapture();
        m_captureEpoch.clear();
        m_captureRetryAt = 0;
        if (!m_status.isEmpty()) { m_status.clear(); emit statusChanged(); }
    }
    m_transport->setSharingEnabled(enabled);
    refresh();
}

void AudioSharingService::setSuspended(bool suspended)
{
    if (m_suspended == suspended) return;
    m_suspended = suspended;
    if (suspended) { m_worker->stopCapture(); m_captureEpoch.clear(); }
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
    const bool listen = !m_muted && !m_suspended && !m_endpoint.isEmpty()
        && m_network->isConnected() && m_transport->isSupported() && binding.active
        && m_network->canIssueSessionCommands(binding.remoteSessionId);
    if (!listen) {
        m_playbackAllowed = false;
        if (!m_session.isEmpty()) {
            m_transport->setSubscription(m_session, m_generation, false);
            clearPlayback();
            m_session.clear(); m_generation = 0; m_stream.clear();
        }
        setState(m_muted ? QStringLiteral("muted") : QStringLiteral("unavailable"));
    } else if (m_session != binding.remoteSessionId || m_generation != binding.generation) {
        m_playbackAllowed = false;
        clearPlayback();
        m_session = binding.remoteSessionId;
        m_generation = binding.generation;
        m_stream.clear();
        m_worker->setPlaybackMuted(false);
        setState(QStringLiteral("loading"));
        m_transport->setSubscription(m_session, m_generation, true);
    }
    const QString epoch = m_transport->publicationId();
    if (m_enabled && !m_suspended && m_transport->isPublishing() && !epoch.isEmpty()) {
        if (m_captureEpoch != epoch && MediaCaptureClock::nowUs() >= m_captureRetryAt) {
            m_captureEpoch = epoch;
            m_worker->startCapture(epoch, m_transport->audioBitrateBps());
        } else if (m_captureEpoch == epoch) m_worker->setCaptureBitrate(m_transport->audioBitrateBps());
    } else {
        if (!m_captureEpoch.isEmpty()) {
            m_captureEpoch.clear();
            m_worker->stopCapture();
        }
        if (!m_status.isEmpty()) { m_status.clear(); emit statusChanged(); }
    }
}

void AudioSharingService::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    m_playbackAllowed = false;
    m_timer.stop();
    clearPlayback();
    m_worker->stopCapture();
    m_transport->stop();
    m_session.clear(); m_stream.clear(); m_captureEpoch.clear();
    setState(QStringLiteral("unavailable"));
}

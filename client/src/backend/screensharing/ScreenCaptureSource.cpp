#include "backend/screensharing/ScreenCaptureSource.h"
#include "backend/screensharing/ScreenCaptureVideoBuffer.h"
#include "backend/screensharing/CaptureTimestampMapper.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include "backend/platform/LocalScreenTopology.h"
#ifdef Q_OS_WIN
#include "backend/platform/WindowCaptureExclusion.h"
#include "backend/screensharing/WindowsScreenCapture.h"
#endif
#ifdef Q_OS_MACOS
#include "backend/screensharing/MacScreenCapture.h"
#endif

#include <QAbstractVideoBuffer>
#include <QElapsedTimer>
#include <QDebug>
#include <QMediaCaptureSession>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QScreen>
#include <QScreenCapture>
#include <QThread>
#include <QVideoSink>
#include <QWaitCondition>
#include <algorithm>

namespace {
struct LayerRequest {
    ScreenStreamProfile profile;
    quint64 epoch = 0;
    bool forceKeyFrame = true;
    bool failed = false;
};
struct CaptureMailbox {
    QMutex mutex;
    QWaitCondition changed;
    QVideoFrame latest;
    CaptureTimestampMapper timestampMapper;
    qint64 capturedAtUs = 0;
    QSize expectedNativeSize;
    quint64 revision = 0;
    quint64 nextLayerEpoch = 0;
    QHash<QString, LayerRequest> layers;
    bool deliveryPending = false;
    bool backpressured = false;
    bool closed = false;
};

QHash<QString, ScreenStreamProfile> normalizedProfiles(const QHash<QString, ScreenStreamProfile>& profiles) {
    QHash<QString, ScreenStreamProfile> result;
    for (const auto& name : {QStringLiteral("main"), QStringLiteral("low")}) {
        const auto it = profiles.constFind(name);
        if (it != profiles.cend()) result.insert(name, it->normalized());
    }
    return result;
}

ScreenStreamProfile nativeProfile(const QHash<QString, ScreenStreamProfile>& profiles) {
    ScreenStreamProfile result;
    result.maximumEdge = 160;
    result.framesPerSecond = 1;
    for (const auto& profile : profiles) {
        result.maximumEdge = std::max(result.maximumEdge, profile.maximumEdge);
        result.framesPerSecond = std::max(result.framesPerSecond, profile.framesPerSecond);
    }
    return result;
}

// Caller holds the mailbox mutex. Epochs never repeat after removing and adding
// a layer, so an already queued callback cannot revive an obsolete encoder.
void updateLayerProfiles(CaptureMailbox& state, const QHash<QString, ScreenStreamProfile>& profiles) {
    for (const auto& name : state.layers.keys())
        if (!profiles.contains(name)) state.layers.remove(name);
    for (auto it = profiles.cbegin(); it != profiles.cend(); ++it) {
        auto existing = state.layers.find(it.key());
        const auto& value = it.value();
        const bool reopen = existing == state.layers.end() || existing->failed
            || existing->profile.maximumEdge != value.maximumEdge
            || existing->profile.framesPerSecond != value.framesPerSecond
            || existing->profile.bitrateBps != value.bitrateBps
            || existing->profile.keyFrameIntervalMs != value.keyFrameIntervalMs
            || existing->profile.softwarePreset != value.softwarePreset;
        if (reopen) state.layers.insert(it.key(), {value, ++state.nextLayerEpoch, true, false});
        else existing->profile = value;
    }
    state.changed.wakeOne();
}

void submitCaptureFrame(CaptureMailbox& state, const QVideoFrame& frame, bool hostTimestamp = false) {
    if (state.closed) return;
    if (frame.isValid() && state.expectedNativeSize.isValid() && frame.size() != state.expectedNativeSize) return;
    state.latest = frame;
    if (frame.isValid()) {
        const auto now = MediaCaptureClock::nowUs();
        state.capturedAtUs = hostTimestamp
            ? CaptureTimestampMapper::hostTimestamp(frame.startTime(), now)
            : state.timestampMapper.map(frame.startTime(), now);
    }
    if (!frame.isValid()) {
        for (auto& layer : state.layers) {
            layer.epoch = ++state.nextLayerEpoch;
            layer.forceKeyFrame = true;
            // A discontinuity also fences any pending error callback. Its
            // replacement encoder must not inherit an unreported failure.
            layer.failed = false;
        }
    }
    ++state.revision;
    state.changed.wakeOne();
}

#ifdef Q_OS_MACOS
QSize captureSize(QScreen* screen, int maximumEdge) {
    QSize result(qRound(screen->geometry().width() * screen->devicePixelRatio()),
                 qRound(screen->geometry().height() * screen->devicePixelRatio()));
    if (result.width() > maximumEdge || result.height() > maximumEdge)
        result.scale(maximumEdge, maximumEdge, Qt::KeepAspectRatio);
    return QSize(std::max(2, result.width() & ~1), std::max(2, result.height() & ~1));
}
#endif

// Neither QScreenCapture's high-refresh native delivery nor a blocked GUI can
// accumulate queued raw frames or H.264 reference frames. The next encode is
// admitted only after the previous packet has been delivered to the service.
class CaptureWorker final : public QThread {
public:
    CaptureWorker(ScreenCaptureSource* owner, std::shared_ptr<CaptureMailbox> mailbox, bool preferHardware = true)
        : m_owner(owner), m_mailbox(std::move(mailbox)), m_preferHardware(preferHardware) {
        setObjectName(QStringLiteral("Screen H264 encoder"));
    }
    void run() override {
        struct EncoderLayer {
            explicit EncoderLayer(bool hardware, quint64 generation) : encoder(hardware), epoch(generation) {}
            ScreenStreamEncoder encoder;
            QString reportedBackend;
            quint64 epoch;
            quint64 encodedRevision = 0;
            qint64 lastEncodeUs = -1000000;
            qint64 lastKeyFrameUs = -30000000;
            qint64 lastMediaUs = -1;
        };
        struct Work {
            QString name;
            ScreenStreamProfile profile;
            std::shared_ptr<EncoderLayer> layer;
            bool forceKeyFrame;
        };
        struct Delivery {
            QString name;
            quint64 epoch;
            QList<ScreenStreamPacket> packets;
            QString error;
            QString backend;
            bool backendChanged = false;
        };
        QHash<QString, std::shared_ptr<EncoderLayer>> encoders;
        QElapsedTimer clock;
        clock.start();
        const auto state = m_mailbox;
        while (true) {
            QVideoFrame frame;
            QList<Work> work;
            QList<std::shared_ptr<EncoderLayer>> retired;
            qint64 timestamp = 0;
            qint64 capturedAtUs = 0;
            {
                QMutexLocker lock(&state->mutex);
                while (true) {
                    if (state->closed) return;
                    for (const auto& name : encoders.keys())
                        if (!state->layers.contains(name)) retired.append(encoders.take(name));
                    timestamp = clock.nsecsElapsed() / 1000;
                    qint64 waitUs = 1000000;
                    if (state->latest.isValid() && !state->deliveryPending && !state->backpressured) {
                        // Stable order gives main the earliest conversion slot;
                        // each layer still owns its independent pacing and GOP.
                        for (const auto& name : {QStringLiteral("main"), QStringLiteral("low")}) {
                            auto request = state->layers.find(name);
                            if (request == state->layers.end() || request->failed) continue;
                            auto& layer = encoders[name];
                            if (!layer || layer->epoch != request->epoch) {
                                if (layer) retired.append(std::move(layer));
                                layer = std::make_shared<EncoderLayer>(m_preferHardware, request->epoch);
                            }
                            const bool changed = state->revision != layer->encodedRevision
                                || request->forceKeyFrame || layer->encoder.hasDelayedKeyFrame();
                            const qint64 interval = changed ? 1000000 / request->profile.framesPerSecond
                                : qint64(request->profile.idleIntervalMs) * 1000;
                            const qint64 remaining = interval - (timestamp - layer->lastEncodeUs);
                            if (remaining > 0) { waitUs = std::min(waitUs, remaining); continue; }
                            const bool forceKeyFrame = request->forceKeyFrame
                                && timestamp - layer->lastKeyFrameUs
                                    >= qint64(request->profile.minimumKeyFrameIntervalMs) * 1000;
                            if (forceKeyFrame) request->forceKeyFrame = false;
                            layer->encodedRevision = state->revision;
                            layer->lastEncodeUs = timestamp;
                            work.append({name, request->profile, layer, forceKeyFrame});
                        }
                    }
                    if (!work.isEmpty()) {
                        frame = state->latest;
                        capturedAtUs = state->capturedAtUs;
                        state->deliveryPending = true;
                        break;
                    }
                    if (!retired.isEmpty()) {
                        // Closing a hardware encoder can wait for its driver.
                        // Never hold up native capture or the GUI mailbox lock.
                        lock.unlock();
                        retired.clear();
                        lock.relock();
                        continue;
                    }
                    state->changed.wait(&state->mutex,
                        static_cast<unsigned long>(std::max<qint64>(1, (waitUs + 999) / 1000)));
                }
            }
            retired.clear();
            QElapsedTimer encodingClock;
            encodingClock.start();
            QList<Delivery> deliveries;
            for (const auto& item : work) {
                {
                    QMutexLocker lock(&state->mutex);
                    if (state->closed) return;
                    const auto request = state->layers.constFind(item.name);
                    if (request == state->layers.cend() || request->epoch != item.layer->epoch) continue;
                }
                auto& layer = *item.layer;
                layer.encoder.setProfile(item.profile);
                // Wrappers share one immutable raw surface and have their own
                // timing metadata; low conversion never mutates main's input.
                QString error;
                // An idle refresh still describes the current unchanged desktop;
                // do not feed duplicate codec PTS when reusing its native surface.
                const qint64 mediaUs = capturedAtUs > layer.lastMediaUs ? capturedAtUs
                    : std::max(layer.lastMediaUs + 1, MediaCaptureClock::nowUs());
                layer.lastMediaUs = mediaUs;
                auto packets = layer.encoder.encode(presentationFrame(frame, mediaUs), item.forceKeyFrame, error);
                for (auto& packet : packets) {
                    packet.layer = item.name;
                    if (packet.keyFrame) layer.lastKeyFrameUs = timestamp;
                }
                const auto backend = layer.encoder.backendName();
                const bool backendChanged = backend != layer.reportedBackend;
                if (backendChanged) layer.reportedBackend = backend;
                if (!error.isEmpty()) {
                    QMutexLocker lock(&state->mutex);
                    auto request = state->layers.find(item.name);
                    if (request != state->layers.end() && request->epoch == layer.epoch) request->failed = true;
                }
                if (!packets.isEmpty() || !error.isEmpty() || backendChanged)
                    deliveries.append({item.name, layer.epoch, std::move(packets), error, backend, backendChanged});
            }
            const int encodingMs = int(std::min<qint64>(60000, encodingClock.elapsed()));
            {
                QMutexLocker lock(&state->mutex);
                if (state->closed) return;
                if (deliveries.isEmpty()) {
                    state->deliveryPending = false;
                    state->changed.wakeOne();
                    continue;
                }
            }
            QMetaObject::invokeMethod(m_owner, [owner = m_owner, state, deliveries = std::move(deliveries), encodingMs]() {
                const auto current = [&state](const Delivery& delivery) {
                    QMutexLocker lock(&state->mutex);
                    const auto request = state->layers.constFind(delivery.name);
                    return !state->closed && request != state->layers.cend() && request->epoch == delivery.epoch;
                };
                bool measured = false;
                for (const auto& delivery : deliveries) {
                    if (!current(delivery)) continue;
                    if (!measured) { emit owner->encodingMeasured(encodingMs); measured = true; }
                    if (!current(delivery)) continue;
                    if (delivery.backendChanged) emit owner->backendChanged(delivery.backend);
                    for (const auto& packet : delivery.packets) {
                        // A signal handler can remove a layer or stop capture
                        // synchronously. Recheck the exact layer epoch each time.
                        if (!current(delivery)) break;
                        emit owner->packetReady(packet);
                    }
                    if (current(delivery) && !delivery.error.isEmpty()) {
                        emit owner->layerEncodingFailed(delivery.name, delivery.error);
                        if (delivery.name == QLatin1String("main") && current(delivery))
                            emit owner->errorOccurred(ScreenCaptureError::EncodingFailed, delivery.error);
                    }
                }
                QMutexLocker lock(&state->mutex);
                state->deliveryPending = false;
                state->changed.wakeOne();
            }, Qt::QueuedConnection);
        }
    }

private:
    // The capture buffer is shared without copying its pixels. Qt 6.8+'s public
    // buffer API gives this worker separate timing metadata and maps natively.
    class TimedBuffer final : public ScreenCaptureVideoBuffer {
    public:
        explicit TimedBuffer(QVideoFrame source) : m_source(std::move(source)) {}
        ~TimedBuffer() override { unmap(); }
        QVideoFrameFormat format() const override { return m_source.surfaceFormat(); }
        AVFrame* nativeEncoderFrame() const override { return screenCaptureNativeFrame(m_source); }
        MapData map(QVideoFrame::MapMode mode) override {
            if (mode != QVideoFrame::ReadOnly || !m_source.map(mode)) return {};
            m_mapped = true;
            MapData data;
            data.planeCount = m_source.planeCount();
            for (int i = 0; i < data.planeCount && i < 4; ++i) {
                data.data[i] = m_source.bits(i);
                data.bytesPerLine[i] = m_source.bytesPerLine(i);
                data.dataSize[i] = m_source.mappedBytes(i);
            }
            return data;
        }
        void unmap() override { if (m_mapped) { m_source.unmap(); m_mapped = false; } }
    private:
        QVideoFrame m_source;
        bool m_mapped = false;
    };
    static QVideoFrame presentationFrame(QVideoFrame source, qint64 timestamp) {
        const auto rotation = source.rotation();
        const auto mirrored = source.mirrored();
        QVideoFrame frame(std::make_unique<TimedBuffer>(std::move(source)));
        frame.setStartTime(timestamp);
        frame.setRotation(rotation);
        frame.setMirrored(mirrored);
        return frame;
    }
    ScreenCaptureSource* const m_owner;
    std::shared_ptr<CaptureMailbox> m_mailbox;
    const bool m_preferHardware;
};
}

struct ScreenCaptureSource::Private {
#ifdef Q_OS_WIN
    WindowsScreenCapture fallback;
    bool fallbackAttempted = false;
#endif
#ifdef Q_OS_MACOS
    MacScreenCapture capture;
#else
    QScreenCapture capture;
    QMediaCaptureSession session;
    QVideoSink sink;
#endif
    QPointer<QScreen> screen;
    std::shared_ptr<CaptureMailbox> mailbox;
    std::unique_ptr<CaptureWorker> worker;
    QMetaObject::Connection frameConnection;
    QMetaObject::Connection screenConnection;
    QHash<QString, ScreenStreamProfile> profiles{{QStringLiteral("main"), ScreenStreamProfile{}}};
    bool backpressured = false;
};

ScreenCaptureSource::ScreenCaptureSource(QObject* parent) : QObject(parent), d(std::make_unique<Private>()) {
    qRegisterMetaType<ScreenStreamPacket>();
    qRegisterMetaType<ScreenCaptureError>();
#ifdef Q_OS_WIN
    connect(&WindowCaptureExclusion::instance(), &WindowCaptureExclusion::captureSafetyChanged,
            this, [this](bool allowed, const QString& error) {
        if (allowed || !d->mailbox) return;
        // Fence raw and encoded deliveries synchronously, before the new
        // unprotected native window can be included by desktop duplication.
        {
            QMutexLocker lock(&d->mailbox->mutex);
            d->mailbox->closed = true;
            d->mailbox->latest = {};
            d->mailbox->changed.wakeOne();
        }
        QMetaObject::invokeMethod(this, [this, generation = d->mailbox, error] {
            if (d->mailbox != generation) return;
            stop();
            emit errorOccurred(ScreenCaptureError::CaptureFailed, error);
        }, Qt::QueuedConnection);
    });
#endif
#ifndef Q_OS_MACOS
    d->session.setScreenCapture(&d->capture);
    d->session.setVideoSink(&d->sink);
    connect(&d->capture, &QScreenCapture::errorOccurred, this,
        [this](QScreenCapture::Error code, const QString& message) {
            if (code == QScreenCapture::NoError || !d->mailbox) return;
            // Finish the native backend's error callback before stopping it.
            const auto generation = d->mailbox;
            QMetaObject::invokeMethod(this, [this, generation, message] {
                if (d->mailbox != generation) return;
#ifdef Q_OS_WIN
                // Several DXGI errors can already be queued. Only the first
                // may switch backend; later ones must not stop its replacement.
                if (!d->fallbackAttempted) startWindowsFallback(message);
#else
                stop();
                emit errorOccurred(ScreenCaptureError::CaptureFailed, message.isEmpty() ? QStringLiteral("Native screen capture failed") : message);
#endif
            }, Qt::QueuedConnection);
        });
#endif
}
ScreenCaptureSource::~ScreenCaptureSource() { stop(); }

#ifdef Q_OS_WIN
void ScreenCaptureSource::startWindowsFallback(const QString& dxgiError) {
    d->fallbackAttempted = true;
    disconnect(d->frameConnection);
    d->capture.stop();
    d->sink.setVideoFrame({});
    // Preserve the publication and its profiles, but fence the old encoder
    // chain before switching from DXGI's relative timestamps to native QPC.
    const auto mailbox = d->mailbox;
    {
        QMutexLocker lock(&mailbox->mutex);
        submitCaptureFrame(*mailbox, {});
    }
    qWarning().noquote() << "[ScreenSharing] DXGI capture failed; trying Windows Graphics Capture:" << dxgiError;
    d->fallback.setProfile(nativeProfile(d->profiles));
    d->fallback.start(d->screen, [mailbox](const QVideoFrame& frame) {
        QMutexLocker lock(&mailbox->mutex);
        submitCaptureFrame(*mailbox, frame, true);
    }, [this, mailbox, dxgiError](ScreenCaptureError code, const QString& message) {
        QMutexLocker lock(&mailbox->mutex);
        if (mailbox->closed) return;
        QMetaObject::invokeMethod(this, [this, mailbox, dxgiError, code, message] {
            if (d->mailbox != mailbox) return;
            stop();
            emit errorOccurred(code, QStringLiteral("%1; DXGI: %2").arg(message, dxgiError));
        }, Qt::QueuedConnection);
    });
}
#endif

bool ScreenCaptureSource::start(const QString& hardwareIdentity) {
    for (const auto& screen : LocalScreenTopology::screens())
        if (screen.identity == hardwareIdentity && screen.screen) return start(screen.screen);
    stop();
    emit errorOccurred(ScreenCaptureError::CaptureFailed, QStringLiteral("The screen selected for sharing is no longer connected"));
    return false;
}

bool ScreenCaptureSource::start(QScreen* screen) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (isActive() && d->screen == screen) return true;
    stop();
    if (!screen) { emit errorOccurred(ScreenCaptureError::CaptureFailed, QStringLiteral("No screen was selected for sharing")); return false; }
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    // The generic Qt capture backend cannot exclude an entire application.
    // Never publish a desktop that also contains Mouffette's scene surfaces.
    emit errorOccurred(ScreenCaptureError::CaptureFailed,
        QStringLiteral("Screen sharing is not supported on this platform because it cannot exclude Mouffette from capture."));
    return false;
#endif
#ifdef Q_OS_WIN
    QString exclusionError;
    if (!WindowCaptureExclusion::instance().prepareForCapture(&exclusionError)) {
        emit errorOccurred(ScreenCaptureError::CaptureFailed, exclusionError);
        return false;
    }
#endif
    d->screen = screen;
    d->mailbox = std::make_shared<CaptureMailbox>();
    updateLayerProfiles(*d->mailbox, d->profiles);
    d->mailbox->backpressured = d->backpressured;
#ifdef Q_OS_MACOS
    d->mailbox->expectedNativeSize = captureSize(screen, nativeProfile(d->profiles).maximumEdge);
#endif
    d->worker = std::make_unique<CaptureWorker>(this, d->mailbox);
    const auto submit = [mailbox = d->mailbox](const QVideoFrame& frame) {
            QMutexLocker lock(&mailbox->mutex);
            submitCaptureFrame(*mailbox, frame,
#ifdef Q_OS_MACOS
                true
#else
                false
#endif
            );
        };
#ifndef Q_OS_MACOS
    d->frameConnection = connect(&d->sink, &QVideoSink::videoFrameChanged, this, submit, Qt::DirectConnection);
#endif
    d->screenConnection = connect(screen, &QObject::destroyed, this, [this] {
        stop();
        emit errorOccurred(ScreenCaptureError::CaptureFailed, QStringLiteral("The shared screen was disconnected"));
    });
    d->worker->start();
#ifdef Q_OS_MACOS
    d->capture.setProfile(nativeProfile(d->profiles));
    d->capture.start(screen, submit, [this, mailbox = d->mailbox](ScreenCaptureError code, const QString& message) {
        // The mutex fences invoking a GUI callback against destruction. Pixels
        // use the independent mailbox and never require the facade to be alive.
        QMutexLocker lock(&mailbox->mutex);
        if (mailbox->closed) return;
        QMetaObject::invokeMethod(this, [this, mailbox, code, message] {
            if (d->mailbox != mailbox) return;
            stop();
            emit errorOccurred(code, message);
        }, Qt::QueuedConnection);
    });
#else
    d->capture.setScreen(screen);
    d->capture.start();
#endif
    return isActive();
}

bool ScreenCaptureSource::isActive() const {
#ifdef Q_OS_WIN
    if (d->fallbackAttempted) return d->mailbox && d->fallback.isActive();
#endif
    return d->mailbox && d->capture.isActive();
}

void ScreenCaptureSource::setProfile(const ScreenStreamProfile& profile) {
    setProfiles({{QStringLiteral("main"), profile}});
}

void ScreenCaptureSource::setProfiles(const QHash<QString, ScreenStreamProfile>& profiles) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto values = normalizedProfiles(profiles);
#ifdef Q_OS_MACOS
    const auto previousNative = nativeProfile(d->profiles);
    const auto nextNative = nativeProfile(values);
#endif
    d->profiles = values;
    if (d->mailbox) {
        QMutexLocker lock(&d->mailbox->mutex);
        updateLayerProfiles(*d->mailbox, values);
#ifdef Q_OS_MACOS
        // A native resize need not restart unchanged lower encoders. Only
        // discard a raw surface whose dimensions no longer match capture.
        if (previousNative.maximumEdge != nextNative.maximumEdge && d->screen) {
            d->mailbox->expectedNativeSize = captureSize(d->screen, nextNative.maximumEdge);
            if (d->mailbox->latest.size() != d->mailbox->expectedNativeSize) d->mailbox->latest = {};
        }
#endif
    }
#ifdef Q_OS_MACOS
    d->capture.setProfile(nextNative);
#endif
#ifdef Q_OS_WIN
    d->fallback.setProfile(nativeProfile(values));
#endif
}

void ScreenCaptureSource::setBackpressured(bool backpressured) {
    Q_ASSERT(QThread::currentThread() == thread());
    d->backpressured = backpressured;
    if (!d->mailbox) return;
    QMutexLocker lock(&d->mailbox->mutex);
    if (d->mailbox->backpressured == backpressured) return;
    d->mailbox->backpressured = backpressured;
    d->mailbox->changed.wakeOne();
}

void ScreenCaptureSource::requestKeyFrame(const QString& layer) {
    if (!d->mailbox) return;
    QMutexLocker lock(&d->mailbox->mutex);
    for (auto it = d->mailbox->layers.begin(); it != d->mailbox->layers.end(); ++it)
        if (layer.isEmpty() || it.key() == layer) it->forceKeyFrame = true;
    d->mailbox->changed.wakeOne();
}

void ScreenCaptureSource::stop() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->mailbox) {
        QMutexLocker lock(&d->mailbox->mutex);
        d->mailbox->closed = true;
        d->mailbox->latest = {};
        d->mailbox->changed.wakeOne();
    }
    disconnect(d->frameConnection);
    disconnect(d->screenConnection);
#ifdef Q_OS_WIN
    d->fallback.stop();
    d->fallbackAttempted = false;
#endif
    d->capture.stop();
#ifndef Q_OS_MACOS
    d->sink.setVideoFrame({});
#endif
    if (d->worker) d->worker->wait();
    d->worker.reset();
    d->mailbox.reset();
    d->screen.clear();
}

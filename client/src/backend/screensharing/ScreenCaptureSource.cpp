#include "backend/screensharing/ScreenCaptureSource.h"
#include "backend/screensharing/ScreenCaptureVideoBuffer.h"
#include "backend/platform/LocalScreenTopology.h"
#ifdef Q_OS_MACOS
#include "backend/screensharing/MacScreenCapture.h"
#endif

#include <QAbstractVideoBuffer>
#include <QElapsedTimer>
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
struct CaptureMailbox {
    QMutex mutex;
    QWaitCondition changed;
    QVideoFrame latest;
    quint64 revision = 0;
    quint64 contentEpoch = 0;
    bool forceKeyFrame = true;
    bool deliveryPending = false;
    bool closed = false;
};

// Neither QScreenCapture's high-refresh native delivery nor a blocked GUI can
// accumulate queued raw frames or H.264 reference frames. The next encode is
// admitted only after the previous packet has been delivered to the service.
class CaptureWorker final : public QThread {
public:
    CaptureWorker(ScreenCaptureSource* owner, std::shared_ptr<CaptureMailbox> mailbox)
        : m_owner(owner), m_mailbox(std::move(mailbox)) { setObjectName(QStringLiteral("Screen H264 encoder")); }
    void run() override {
        ScreenStreamEncoder encoder;
        QString reportedBackend;
        QElapsedTimer clock;
        clock.start();
        quint64 encodedRevision = 0;
        quint64 encodedEpoch = 0;
        qint64 lastEncodeUs = -1000000;
        const auto state = m_mailbox;
        while (true) {
            QVideoFrame frame;
            bool forceKeyFrame;
            qint64 timestamp;
            quint64 contentEpoch;
            {
                QMutexLocker lock(&state->mutex);
                while (true) {
                    if (state->closed) return;
                    timestamp = clock.nsecsElapsed() / 1000;
                    const qint64 elapsed = timestamp - lastEncodeUs;
                    const bool changed = state->revision != encodedRevision || state->forceKeyFrame || encoder.hasDelayedKeyFrame();
                    const qint64 interval = changed ? 1000000 / ScreenStreamEncoder::FramesPerSecond : 1000000;
                    if (state->latest.isValid() && !state->deliveryPending && elapsed >= interval) break;
                    const auto waitMs = !state->latest.isValid() || state->deliveryPending
                        ? 1000ul : static_cast<unsigned long>(std::max<qint64>(1, (interval - elapsed + 999) / 1000));
                    state->changed.wait(&state->mutex, waitMs);
                }
                frame = state->latest;
                contentEpoch = state->contentEpoch;
                encodedRevision = state->revision;
                forceKeyFrame = state->forceKeyFrame;
                state->forceKeyFrame = false;
            }
            if (encodedEpoch != contentEpoch) {
                encoder.reset(); forceKeyFrame = true; encodedEpoch = contentEpoch;
            }
            // QVideoFrame is explicitly shared. Do not mutate the native sink's
            // timestamps: a separate presentation wrapper owns this metadata.
            frame = presentationFrame(std::move(frame), timestamp);
            lastEncodeUs = timestamp;
            QString error;
            auto packets = encoder.encode(std::move(frame), forceKeyFrame, error);
            const auto backend = encoder.backendName();
            const bool backendChanged = backend != reportedBackend;
            if (backendChanged) reportedBackend = backend;
            if (packets.isEmpty() && error.isEmpty() && !backendChanged) continue;
            {
                QMutexLocker lock(&state->mutex);
                if (state->closed) return;
                if (state->contentEpoch != contentEpoch) continue;
                state->deliveryPending = true;
            }
            QMetaObject::invokeMethod(m_owner, [owner = m_owner, state, packets = std::move(packets), error,
                                               backend, backendChanged, contentEpoch]() {
                {
                    QMutexLocker lock(&state->mutex);
                    if (state->closed) return;
                    if (state->contentEpoch != contentEpoch) {
                        state->deliveryPending = false; state->changed.wakeOne(); return;
                    }
                }
                if (backendChanged) emit owner->backendChanged(backend);
                for (const auto& packet : packets) {
                    // A signal handler can disable sharing synchronously.
                    {
                        QMutexLocker lock(&state->mutex);
                        if (state->closed) return;
                        if (state->contentEpoch != contentEpoch) {
                            state->deliveryPending = false; state->changed.wakeOne(); return;
                        }
                    }
                    emit owner->packetReady(packet);
                }
                {
                    QMutexLocker lock(&state->mutex);
                    if (state->closed) return;
                    if (state->contentEpoch != contentEpoch) {
                        state->deliveryPending = false; state->changed.wakeOne(); return;
                    }
                }
                if (!error.isEmpty()) {
                    owner->stop();
                    emit owner->errorOccurred(ScreenCaptureError::CaptureFailed, error);
                    return;
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
};
}

struct ScreenCaptureSource::Private {
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
};

ScreenCaptureSource::ScreenCaptureSource(QObject* parent) : QObject(parent), d(std::make_unique<Private>()) {
    qRegisterMetaType<ScreenStreamPacket>();
    qRegisterMetaType<ScreenCaptureError>();
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
                stop();
                emit errorOccurred(ScreenCaptureError::CaptureFailed, message.isEmpty() ? QStringLiteral("Native screen capture failed") : message);
            }, Qt::QueuedConnection);
        });
#endif
}
ScreenCaptureSource::~ScreenCaptureSource() { stop(); }

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
    d->screen = screen;
    d->mailbox = std::make_shared<CaptureMailbox>();
    d->worker = std::make_unique<CaptureWorker>(this, d->mailbox);
    const auto submit = [mailbox = d->mailbox](const QVideoFrame& frame) {
            QMutexLocker lock(&mailbox->mutex);
            if (mailbox->closed) return;
            mailbox->latest = frame;
            if (!frame.isValid()) { ++mailbox->contentEpoch; mailbox->forceKeyFrame = true; }
            ++mailbox->revision;
            mailbox->changed.wakeOne();
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

bool ScreenCaptureSource::isActive() const { return d->mailbox && d->capture.isActive(); }

void ScreenCaptureSource::requestKeyFrame() {
    if (!d->mailbox) return;
    QMutexLocker lock(&d->mailbox->mutex);
    d->mailbox->forceKeyFrame = true;
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
    d->capture.stop();
#ifndef Q_OS_MACOS
    d->sink.setVideoFrame({});
#endif
    if (d->worker) d->worker->wait();
    d->worker.reset();
    d->mailbox.reset();
    d->screen.clear();
}

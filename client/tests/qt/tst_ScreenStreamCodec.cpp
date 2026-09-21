#include "backend/screensharing/ScreenStreamCodec.h"

#include <QColor>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QtTest>
#include <cstring>
#ifdef Q_OS_MACOS
#include "backend/screensharing/MacScreenCapture.h"
#include <CoreVideo/CoreVideo.h>
#include <CoreGraphics/CoreGraphics.h>
#endif

class ScreenStreamCodecTest final : public QObject {
    Q_OBJECT
private:
    static QVideoFrame frame(QSize size, qint64 timestamp, int surfaceRotation = 0, bool surfaceMirror = false) {
        QVideoFrameFormat format(size, QVideoFrameFormat::Format_BGRA8888);
        format.setRotation(QtVideo::Rotation(surfaceRotation)); format.setMirrored(surfaceMirror);
        QVideoFrame result(format);
        if (!result.map(QVideoFrame::WriteOnly)) return {};
        for (int y = 0; y < size.height(); ++y) {
            auto* row = result.bits(0) + y * result.bytesPerLine(0);
            for (int x = 0; x < size.width(); ++x) {
                row[x * 4 + 0] = 40;
                row[x * 4 + 1] = x < size.width() / 2 ? 70 : 180;
                row[x * 4 + 2] = x < size.width() / 2 ? 220 : 50;
                row[x * 4 + 3] = 255;
            }
        }
        result.unmap(); result.setStartTime(timestamp);
        return result;
    }

    static QColor sample709(QVideoFrame frame, int x, int y) {
        if (!frame.map(QVideoFrame::ReadOnly)) return {};
        const double luma = (frame.bits(0)[y * frame.bytesPerLine(0) + x] - 16) * 255.0 / 219.0;
        const double u = (frame.bits(1)[(y / 2) * frame.bytesPerLine(1) + x / 2] - 128) * 255.0 / 224.0;
        const double v = (frame.bits(2)[(y / 2) * frame.bytesPerLine(2) + x / 2] - 128) * 255.0 / 224.0;
        frame.unmap();
        return QColor(qBound(0, qRound(luma + 1.5748 * v), 255),
            qBound(0, qRound(luma - 0.1873 * u - 0.4681 * v), 255), qBound(0, qRound(luma + 1.8556 * u), 255));
    }

private slots:
    void roundTrip_data() {
        QTest::addColumn<QSize>("source");
        QTest::addColumn<QSize>("expected");
        QTest::newRow("landscape") << QSize(640, 360) << QSize(640, 360);
        QTest::newRow("retina") << QSize(3840, 2160) << QSize(1920, 1080);
        QTest::newRow("portrait") << QSize(1080, 3840) << QSize(540, 1920);
        QTest::newRow("odd") << QSize(641, 361) << QSize(640, 360);
    }

    void roundTrip() {
        QFETCH(QSize, source);
        QFETCH(QSize, expected);
        ScreenStreamEncoder encoder(false);
        ScreenStreamDecoder decoder;
        QString error;
        const auto packets = encoder.encode(frame(source, 1234567), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets.first().size, expected);
        QVERIFY(packets.first().keyFrame);
        QCOMPARE(packets.first().timestampUs, 1234567);
        auto decoded = decoder.decode(packets.first().annexB, packets.first().timestampUs, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(decoded.isValid());
        QCOMPARE(decoded.size(), expected);
        QCOMPARE(decoded.pixelFormat(), QVideoFrameFormat::Format_YUV420P);
        QCOMPARE(decoded.startTime(), 1234567);
        QVERIFY(decoded.map(QVideoFrame::ReadOnly));
        QCOMPARE(decoded.planeCount(), 3);
        QVERIFY(decoded.mappedBytes(0) >= expected.width() * expected.height());
        decoded.unmap();
        // Qt's CPU toImage() fallback assumes BT.601. The canvas GPU shader
        // respects these metadata; verify BT.709 directly without that fallback.
        QCOMPARE(decoded.surfaceFormat().colorSpace(), QVideoFrameFormat::ColorSpace_BT709);
        QCOMPARE(decoded.surfaceFormat().colorRange(), QVideoFrameFormat::ColorRange_Video);
        const auto left = sample709(decoded, expected.width() / 4, expected.height() / 2);
        const auto right = sample709(decoded, 3 * expected.width() / 4, expected.height() / 2);
        QVERIFY(qAbs(left.red() - 220) < 20);
        QVERIFY(qAbs(left.green() - 70) < 20);
        QVERIFY(qAbs(left.blue() - 40) < 20);
        QVERIFY(qAbs(right.red() - 50) < 20);
        QVERIFY2(qAbs(right.green() - 180) < 20, qPrintable(QStringLiteral("Right sample %1,%2,%3").arg(right.red()).arg(right.green()).arg(right.blue())));
    }

    void requestedKeyframeRestartsFreshDecoder() {
        ScreenStreamEncoder encoder(false);
        QString error;
        for (int i = 0; i < 8; ++i) {
            const auto packets = encoder.encode(frame(QSize(320, 180), i * 33333), i == 7, error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(packets.size(), 1);
            QCOMPARE(packets.first().keyFrame, i == 0 || i == 7);
            if (i == 7) {
                ScreenStreamDecoder freshDecoder;
                QVERIFY(freshDecoder.decode(packets.first().annexB, packets.first().timestampUs, error).isValid());
                QVERIFY2(error.isEmpty(), qPrintable(error));
            }
        }
    }

    void displayResizeRestartsStream() {
        ScreenStreamEncoder encoder(false);
        QString error;
        QVERIFY(!encoder.encode(frame(QSize(320, 180), 0), false, error).isEmpty());
        auto packets = encoder.encode(frame(QSize(180, 320), 33333), false, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        QVERIFY(packets.first().keyFrame);
        ScreenStreamDecoder decoder;
        QCOMPARE(decoder.decode(packets.first().annexB, packets.first().timestampUs, error).size(), QSize(180, 320));
    }

    void adaptiveProfilesPreserveGeometryAndRestartWithIndependentKeyframes() {
        ScreenStreamEncoder encoder(false);
        ScreenStreamProfile profile;
        QString error;
        qint64 timestamp = 0;
        for (const int maximumEdge : {640, 1280, 3840, 320}) {
            profile.maximumEdge = maximumEdge;
            profile.framesPerSecond = maximumEdge <= 640 ? 10 : 30;
            profile.bitrateBps = maximumEdge <= 640 ? 300000 : 6000000;
            encoder.setProfile(profile);
            const auto packets = encoder.encode(frame(QSize(3840, 2160), timestamp), false, error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(packets.size(), 1);
            QVERIFY(packets.first().keyFrame);
            QCOMPARE(packets.first().size, QSize(maximumEdge, maximumEdge * 9 / 16));
            ScreenStreamDecoder decoder;
            QCOMPARE(decoder.decode(packets.first().annexB, timestamp, error).size(), packets.first().size);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            timestamp += 100000;
        }
    }

    void rateChangeIsAppliedAtKeyframeAndIdenticalProfileDoesNotRestart() {
        ScreenStreamEncoder encoder(false);
        ScreenStreamProfile profile;
        profile.maximumEdge = 640;
        profile.bitrateBps = 1000000;
        QString error;
        encoder.setProfile(profile);
        auto packets = encoder.encode(frame(QSize(640, 360), 0), false, error);
        QCOMPARE(packets.size(), 1);
        QVERIFY(packets.first().keyFrame);
        encoder.setProfile(profile);
        packets = encoder.encode(frame(QSize(640, 360), 33333), false, error);
        QCOMPARE(packets.size(), 1);
        QVERIFY(!packets.first().keyFrame);
        profile.bitrateBps = 200000;
        encoder.setProfile(profile);
        packets = encoder.encode(frame(QSize(640, 360), 66666), false, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        QVERIFY(packets.first().keyFrame);
        ScreenStreamDecoder fresh;
        QVERIFY(fresh.decode(packets.first().annexB, 66666, error).isValid());
        encoder.reset();
        packets = encoder.encode(frame(QSize(1280, 720), 99999), false, error);
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets.first().size, QSize(640, 360));
    }

    void stationaryFreshnessDoesNotForceAKeyframeEveryTwoSeconds() {
        ScreenStreamEncoder encoder(false);
        ScreenStreamProfile profile;
        profile.keyFrameIntervalMs = 10000;
        encoder.setProfile(profile);
        QString error;
        for (int second = 0; second <= 10; ++second) {
            const auto packets = encoder.encode(frame(QSize(320, 180), second * 1000000LL), false, error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(packets.size(), 1);
            QCOMPARE(packets.first().keyFrame, second == 0 || second == 10);
        }
    }

    void bitrateBudgetActuallyReducesEncodedTraffic() {
        const auto encodedBytes = [](int bitrate) {
            ScreenStreamEncoder encoder(false);
            ScreenStreamProfile profile;
            profile.bitrateBps = bitrate;
            profile.keyFrameIntervalMs = 10000;
            encoder.setProfile(profile);
            qint64 bytes = 0;
            quint32 noise = 12345;
            for (int index = 0; index < 60; ++index) {
                QVideoFrame sample(QVideoFrameFormat(QSize(320, 180), QVideoFrameFormat::Format_BGRA8888));
                if (!sample.map(QVideoFrame::WriteOnly)) return qint64(-1);
                for (int y = 0; y < sample.height(); ++y) {
                    auto* row = sample.bits(0) + y * sample.bytesPerLine(0);
                    for (int x = 0; x < sample.width(); ++x) {
                        noise = noise * 1664525u + 1013904223u;
                        row[x * 4] = uchar(noise);
                        row[x * 4 + 1] = uchar(noise >> 8);
                        row[x * 4 + 2] = uchar(noise >> 16);
                        row[x * 4 + 3] = 255;
                    }
                }
                sample.unmap();
                sample.setStartTime(index * 33333LL);
                QString error;
                const auto packets = encoder.encode(sample, false, error);
                if (!error.isEmpty() || packets.isEmpty()) return qint64(-1);
                for (const auto& packet : packets) bytes += packet.annexB.size();
            }
            return bytes;
        };
        const auto high = encodedBytes(2000000);
        const auto low = encodedBytes(250000);
        QVERIFY(high > 0);
        QVERIFY(low > 0);
        QVERIFY2(low < high / 2, qPrintable(QStringLiteral("Low-rate bytes %1, high-rate bytes %2").arg(low).arg(high)));
        // The finite two-second run includes its initial IDR and encoder burst.
        QVERIFY2(low * 8 / 2 < 500000, "The low-rate encoder must honor a bounded traffic budget");
    }

    void invalidProfilesStayInsideCodecSafetyLimits() {
        ScreenStreamProfile profile;
        profile.maximumEdge = 999999;
        profile.framesPerSecond = 0;
        profile.bitrateBps = -1;
        profile.idleIntervalMs = 999999;
        profile.keyFrameIntervalMs = -1;
        profile.minimumKeyFrameIntervalMs = -1;
        profile.softwarePreset = QStringLiteral("untrusted-option");
        const auto bounded = profile.normalized();
        QCOMPARE(bounded.maximumEdge, ScreenStreamEncoder::MaximumDecodeEdge);
        QCOMPARE(bounded.framesPerSecond, 1);
        QCOMPARE(bounded.bitrateBps, 32000);
        QCOMPARE(bounded.idleIntervalMs, 4000);
        QCOMPARE(bounded.keyFrameIntervalMs, 500);
        QCOMPARE(bounded.minimumKeyFrameIntervalMs, 100);
        QCOMPARE(bounded.softwarePreset, QStringLiteral("veryfast"));
    }

    void decodedFrameOutlivesDecoder() {
        QVideoFrame decoded;
        QString error;
        {
            ScreenStreamEncoder encoder(false);
            ScreenStreamDecoder decoder;
            const auto packets = encoder.encode(frame(QSize(320, 180), 0), true, error);
            QVERIFY(!packets.isEmpty());
            decoded = decoder.decode(packets.first().annexB, 0, error);
        }
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(decoded.map(QVideoFrame::ReadOnly));
        QVERIFY(decoded.bits(0)[0] > 0);
        decoded.unmap();
    }

    void malformedPacketsAreRejectedAndRecoveryWorks() {
        ScreenStreamDecoder decoder;
        QString error;
        QVERIFY(!decoder.decode(QByteArray(), 0, error).isValid());
        QVERIFY(!error.isEmpty());
        QVERIFY(!decoder.decode(QByteArray(ScreenStreamEncoder::MaximumPacketBytes + 1, 'x'), 0, error).isValid());
        QVERIFY(!error.isEmpty());
        QVERIFY(!decoder.decode(QByteArray("not an H264 access unit"), 0, error).isValid());
        QVERIFY(!error.isEmpty());
        ScreenStreamEncoder encoder(false);
        const auto packets = encoder.encode(frame(QSize(320, 180), 0), true, error);
        QVERIFY(!packets.isEmpty());
        QVERIFY(decoder.decode(packets.first().annexB, 0, error).isValid());
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void portraitRotationAndMirroring_data() {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<bool>("mirror");
        QTest::addColumn<bool>("surface");
        for (int rotation : {0, 90, 180, 270})
            for (bool mirror : {false, true})
                for (bool surface : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("%1-%2-surface%3").arg(rotation).arg(mirror).arg(surface))) << rotation << mirror << surface;
    }

    void portraitRotationAndMirroring() {
        QFETCH(int, rotation); QFETCH(bool, mirror);
        QFETCH(bool, surface);
        auto source = frame(QSize(640, 360), 0, surface ? rotation : 0, surface && mirror);
        if (!surface) { source.setRotation(QtVideo::Rotation(rotation)); source.setMirrored(mirror); }
        ScreenStreamEncoder encoder(false);
        ScreenStreamDecoder decoder;
        QString error;
        const auto packets = encoder.encode(source, true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        auto decoded = decoder.decode(packets.first().annexB, 0, error);
        const bool transposed = rotation == 90 || rotation == 270;
        QCOMPARE(decoded.size(), transposed ? QSize(360, 640) : QSize(640, 360));
        int x = 160, y = 180;
        switch (rotation) {
        case 90: x = 179; y = 160; break;
        case 180: x = 479; y = 179; break;
        case 270: x = 180; y = 479; break;
        }
        if (mirror) x = decoded.width() - 1 - x;
        const auto red = sample709(decoded, x, y);
        QVERIFY(qAbs(red.red() - 220) < 20);
        QVERIFY(qAbs(red.green() - 70) < 20);
    }

    void nativeEncoder1080pSmoke() {
        ScreenStreamEncoder encoder;
        ScreenStreamDecoder decoder;
        auto source = frame(QSize(1920, 1080), 0);
        QString error;
        qint64 encodeNs = 0, decodeNs = 0;
        int decodedCount = 0;
        for (int i = 0; i < 30; ++i) {
            source.setStartTime(i * 33333);
            QElapsedTimer timer; timer.start();
            const auto packets = encoder.encode(source, i == 15, error);
            encodeNs += timer.nsecsElapsed();
            QVERIFY2(error.isEmpty(), qPrintable(error));
            for (const auto& packet : packets) {
                if (i == 15) { QVERIFY(packet.keyFrame); decoder.reset(); }
                timer.restart();
                const auto decoded = decoder.decode(packet.annexB, packet.timestampUs, error);
                decodeNs += timer.nsecsElapsed();
                QVERIFY2(error.isEmpty(), qPrintable(error));
                QVERIFY(decoded.isValid());
                QCOMPARE(decoded.size(), QSize(1920, 1080));
                ++decodedCount;
            }
        }
        QCOMPARE(decodedCount, 30);
        qInfo().noquote() << QStringLiteral("1080p %1: encode+scale %2 ms/frame; decode %3 ms/frame (synthetic stationary desktop)")
            .arg(encoder.backendName()).arg(encodeNs / 30.0 / 1000000.0, 0, 'f', 2)
            .arg(decodeNs / 30.0 / 1000000.0, 0, 'f', 2);
    }

#ifdef Q_OS_MACOS
    void nativePermissionErrorClassification() {
        const auto domain = QStringLiteral("com.apple.ScreenCaptureKit.SCStreamErrorDomain");
        QCOMPARE(MacScreenCapture::nativeErrorCode(domain, -3801), ScreenCaptureError::PermissionDenied);
        QCOMPARE(MacScreenCapture::nativeErrorCode(domain, -3802), ScreenCaptureError::CaptureFailed);
        QCOMPARE(MacScreenCapture::nativeErrorCode(domain, -3817), ScreenCaptureError::CaptureFailed);
        QCOMPARE(MacScreenCapture::nativeErrorCode(QStringLiteral("other.domain"), -3801), ScreenCaptureError::CaptureFailed);
        QCOMPARE(MacScreenCapture::nativeErrorCode({}, 0), ScreenCaptureError::CaptureFailed);
    }

    void nativeTransientSamplesAreNotFailures() {
        using Action = MacScreenCapture::SampleAction;
        // SCFrameStatus values are stable API constants from SCStream.h.
        QCOMPARE(MacScreenCapture::sampleAction(0), Action::Deliver); // Complete
        QCOMPARE(MacScreenCapture::sampleAction(1), Action::Ignore); // Idle
        QCOMPARE(MacScreenCapture::sampleAction(2), Action::Suspend); // Blank
        QCOMPARE(MacScreenCapture::sampleAction(3), Action::Suspend); // Suspended
        QCOMPARE(MacScreenCapture::sampleAction(4), Action::Ignore); // Started, metadata only
        QCOMPARE(MacScreenCapture::sampleAction(5), Action::Stop); // Stopped
        QCOMPARE(MacScreenCapture::sampleAction(-1), Action::Ignore);
    }

    void nativeDesktopSmokeWhenExplicitlyEnabled() {
        if (qEnvironmentVariableIntValue("MOUFFETTE_TEST_SCREEN_CAPTURE") != 1)
            QSKIP("Desktop capture smoke requires explicit MOUFFETTE_TEST_SCREEN_CAPTURE=1");
        if (QGuiApplication::platformName() != QStringLiteral("cocoa")) QSKIP("Requires native Cocoa platform");
        if (!CGPreflightScreenCaptureAccess()) QSKIP("Screen recording access is not already granted; no permission prompt requested");
        struct Mailbox { QMutex mutex; QVideoFrame latest; QString error; int received = 0; };
        const auto mailbox = std::make_shared<Mailbox>();
        MacScreenCapture capture;
        QVERIFY(capture.start(QGuiApplication::primaryScreen(), [mailbox](const QVideoFrame& frame) {
            QMutexLocker lock(&mailbox->mutex); mailbox->latest = frame; ++mailbox->received;
        }, [mailbox](ScreenCaptureError, const QString& message) {
            QMutexLocker lock(&mailbox->mutex); mailbox->error = message;
        }));
        QElapsedTimer timeout; timeout.start();
        QVideoFrame captured;
        QString error;
        while (timeout.elapsed() < 5000) {
            QTest::qWait(20);
            QMutexLocker lock(&mailbox->mutex);
            error = mailbox->error; captured = mailbox->latest;
            if (!error.isEmpty() || captured.isValid()) break;
        }
        capture.stop();
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(captured.isValid());
        QVERIFY(captured.width() <= ScreenStreamEncoder::MaximumEdge);
        QVERIFY(captured.height() <= ScreenStreamEncoder::MaximumEdge);
        ScreenStreamEncoder encoder;
        ScreenStreamDecoder decoder;
        int decoded = 0;
        for (int i = 0; i < 5; ++i) {
            captured.setStartTime(i * 33333);
            const auto packets = encoder.encode(captured, i == 0, error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            for (const auto& packet : packets) {
                const auto video = decoder.decode(packet.annexB, packet.timestampUs, error);
                QVERIFY2(error.isEmpty(), qPrintable(error));
                QVERIFY(video.isValid()); ++decoded;
            }
            QTest::qWait(34);
        }
        QVERIFY(decoded > 0);
        qInfo() << "ScreenCaptureKit live smoke: decoded" << decoded << "samples with" << encoder.backendName();
        // Captured samples are never converted to screenshots or persisted.
    }

    void nativeSurface1080pSmoke() {
        const auto empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
        const void* values[] = {empty};
        const auto attributes = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferRef surface = nullptr;
        const auto status = CVPixelBufferCreate(nullptr, 1920, 1080, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &surface);
        CFRelease(attributes); CFRelease(empty);
        QCOMPARE(status, kCVReturnSuccess);
        QVERIFY(surface);
        QCOMPARE(CVPixelBufferLockBaseAddress(surface, 0), kCVReturnSuccess);
        memset(CVPixelBufferGetBaseAddressOfPlane(surface, 0), 96, CVPixelBufferGetBytesPerRowOfPlane(surface, 0) * CVPixelBufferGetHeightOfPlane(surface, 0));
        memset(CVPixelBufferGetBaseAddressOfPlane(surface, 1), 128, CVPixelBufferGetBytesPerRowOfPlane(surface, 1) * CVPixelBufferGetHeightOfPlane(surface, 1));
        CVPixelBufferUnlockBaseAddress(surface, 0);
        auto source = MacScreenCapture::frameFromPixelBuffer(surface);
        CVPixelBufferRelease(surface); // The production QVideoFrame retains it.
        QVERIFY(source.isValid());
        QCOMPARE(source.pixelFormat(), QVideoFrameFormat::Format_NV12);
        QVERIFY(source.map(QVideoFrame::ReadOnly));
        QCOMPARE(source.bits(0)[0], 96);
        source.unmap();
        ScreenStreamEncoder encoder;
        ScreenStreamDecoder decoder;
        QString error;
        qint64 encodeNs = 0, decodeNs = 0;
        int decodedCount = 0;
        bool receivedRequestedKey = false;
        for (int i = 0; i < 30; ++i) {
            source.setStartTime(i * 33333);
            QElapsedTimer timer; timer.start();
            const auto packets = encoder.encode(source, i == 15, error);
            encodeNs += timer.nsecsElapsed();
            QVERIFY2(error.isEmpty(), qPrintable(error));
            for (const auto& packet : packets) {
                if (packet.timestampUs == 15 * 33333) {
                    QVERIFY(packet.keyFrame); decoder.reset(); receivedRequestedKey = true;
                    QVERIFY(i <= 17);
                }
                timer.restart();
                auto decoded = decoder.decode(packet.annexB, packet.timestampUs, error);
                decodeNs += timer.nsecsElapsed();
                QVERIFY2(error.isEmpty(), qPrintable(error));
                QCOMPARE(decoded.size(), QSize(1920, 1080));
                QVERIFY(decoded.map(QVideoFrame::ReadOnly));
                QVERIFY(qAbs(decoded.bits(0)[0] - 96) < 3);
                decoded.unmap();
                ++decodedCount;
            }
            if (i >= 2) QVERIFY(decodedCount > 0);
            // Native VideoToolbox's completion is asynchronous. Real capture
            // supplies frames at 30 Hz, including a followup for a pending IDR.
            QTest::qWait(34);
        }
        QVERIFY(receivedRequestedKey);
        QVERIFY(decodedCount >= 28);
        qInfo().noquote() << QStringLiteral("1080p IOSurface %1: encode %2 ms/frame; decode %3 ms/frame")
            .arg(encoder.backendName()).arg(encodeNs / 30.0 / 1000000.0, 0, 'f', 2)
            .arg(decodeNs / double(decodedCount) / 1000000.0, 0, 'f', 2);
        // Force the software fallback through the same native capture buffer.
        ScreenStreamEncoder software(false);
        const auto fallback = software.encode(source, true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(fallback.size(), 1);
        decoder.reset();
        QVERIFY(decoder.decode(fallback.first().annexB, 0, error).isValid());
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
#endif
};

QTEST_MAIN(ScreenStreamCodecTest)
#include "tst_ScreenStreamCodec.moc"

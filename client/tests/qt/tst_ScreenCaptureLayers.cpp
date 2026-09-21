// Compile the implementation here to drive its actual bounded worker mailbox
// with synthetic raw frames. No production capture hook or desktop permission.
#include "backend/screensharing/ScreenCaptureSource.cpp"

#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <cstring>

// This target never starts native capture or enumerates the desktop.
namespace LocalScreenTopology {
QList<Screen> screens(bool, bool* success) {
    if (success) *success = true;
    return {};
}
}

namespace {
ScreenStreamProfile profile(int edge, int fps = 60) {
    ScreenStreamProfile result;
    result.maximumEdge = edge;
    result.framesPerSecond = fps;
    result.bitrateBps = edge == 640 ? 1000000 : 250000;
    result.keyFrameIntervalMs = 10000;
    result.minimumKeyFrameIntervalMs = 100;
    result.idleIntervalMs = 4000;
    result.softwarePreset = QStringLiteral("ultrafast");
    return result;
}
QVideoFrame sample() {
    QVideoFrame result(QVideoFrameFormat(QSize(640, 360), QVideoFrameFormat::Format_BGRA8888));
    if (!result.map(QVideoFrame::WriteOnly)) return {};
    for (int y = 0; y < result.height(); ++y) {
        auto* row = result.bits(0) + y * result.bytesPerLine(0);
        for (int x = 0; x < result.width(); ++x) {
            row[x * 4] = 30;
            row[x * 4 + 1] = x < 320 ? 60 : 170;
            row[x * 4 + 2] = x < 320 ? 200 : 40;
            row[x * 4 + 3] = 255;
        }
    }
    result.unmap();
    return result;
}
struct Harness {
    ScreenCaptureSource owner;
    std::shared_ptr<CaptureMailbox> state = std::make_shared<CaptureMailbox>();
    CaptureWorker worker{&owner, state, false};
    QList<ScreenStreamPacket> packets;
    QStringList errors;
    Harness() {
        QObject::connect(&owner, &ScreenCaptureSource::packetReady, &owner,
            [this](const ScreenStreamPacket& packet) { packets.append(packet); });
        QObject::connect(&owner, &ScreenCaptureSource::layerEncodingFailed, &owner,
            [this](const QString& layer, const QString& message) { errors.append(layer + message); });
    }
    ~Harness() { close(); }
    void close() {
        {
            QMutexLocker lock(&state->mutex);
            state->closed = true;
            state->latest = {};
            state->changed.wakeOne();
        }
        worker.wait();
    }
    void profiles(const QHash<QString, ScreenStreamProfile>& values) {
        QMutexLocker lock(&state->mutex);
        updateLayerProfiles(*state, normalizedProfiles(values));
    }
    void push(const QVideoFrame& frame) {
        QMutexLocker lock(&state->mutex);
        submitCaptureFrame(*state, frame);
    }
    void pause(bool paused) {
        QMutexLocker lock(&state->mutex);
        state->backpressured = paused;
        state->changed.wakeOne();
    }
    int count(const QString& layer) const {
        return int(std::count_if(packets.cbegin(), packets.cend(), [&layer](const auto& packet) {
            return packet.layer == layer;
        }));
    }
    QList<ScreenStreamPacket> layerPackets(const QString& layer) const {
        QList<ScreenStreamPacket> result;
        for (const auto& packet : packets) if (packet.layer == layer) result.append(packet);
        return result;
    }
};
}

class ScreenCaptureLayersTest final : public QObject {
    Q_OBJECT
private slots:
    void sameRawSurfaceProducesTwoIndependentlyDecodableLayers() {
        Harness harness;
        harness.profiles({{"main", profile(640)}, {"low", profile(320)}, {"untrusted-third", profile(160)}});
        harness.worker.start();
        const auto raw = sample();
        QVERIFY(raw.isValid());
        harness.push(raw);
        QTRY_COMPARE(harness.packets.size(), 2);
        QVERIFY(harness.errors.isEmpty());
        QCOMPARE(harness.count("main"), 1);
        QCOMPARE(harness.count("low"), 1);
        for (const auto& packet : harness.packets) {
            QVERIFY(packet.keyFrame);
            QCOMPARE(packet.size, packet.layer == QLatin1String("main") ? QSize(640, 360) : QSize(320, 180));
            ScreenStreamDecoder decoder;
            QString error;
            QCOMPARE(decoder.decode(packet.annexB, packet.timestampUs, error).size(), packet.size);
            QVERIFY2(error.isEmpty(), qPrintable(error));
        }
        QCOMPARE(nativeProfile({{"main", profile(640, 15)}, {"low", profile(320, 30)}}).maximumEdge, 640);
        QCOMPARE(nativeProfile({{"main", profile(640, 15)}, {"low", profile(320, 30)}}).framesPerSecond, 30);
    }

    void removingAndReaddingLowKeepsMainReferenceChain() {
        Harness harness;
        const auto main = profile(640), low = profile(320);
        harness.profiles({{"main", main}, {"low", low}});
        harness.worker.start();
        const auto raw = sample();
        harness.push(raw);
        QTRY_COMPARE(harness.packets.size(), 2);
        harness.profiles({{"main", main}});
        harness.push(raw);
        QTRY_COMPARE(harness.count("main"), 2);
        QCOMPARE(harness.count("low"), 1);
        QVERIFY(!harness.layerPackets("main").last().keyFrame);
        harness.profiles({{"main", main}, {"low", low}});
        harness.push(raw);
        QTRY_COMPARE(harness.count("main"), 3);
        QTRY_COMPARE(harness.count("low"), 2);
        QVERIFY(!harness.layerPackets("main").last().keyFrame);
        QVERIFY(harness.layerPackets("low").last().keyFrame);
        ScreenStreamDecoder decoder;
        QString error;
        for (const auto& packet : harness.layerPackets("main"))
            QVERIFY2(decoder.decode(packet.annexB, packet.timestampUs, error).isValid(), qPrintable(error));
        QVERIFY(harness.errors.isEmpty());
    }

    void changingLowProfileFencesOnlyLowPendingDelivery() {
        Harness harness;
        const auto main = profile(640), low = profile(320);
        harness.profiles({{"main", main}, {"low", low}});
        // The first main packet reconfigures low synchronously, while the same
        // GUI callback still owns the original low packet. That packet is stale.
        QObject::connect(&harness.owner, &ScreenCaptureSource::packetReady, &harness.owner,
            [&harness, main](const ScreenStreamPacket& packet) {
                if (packet.layer == QLatin1String("main") && harness.count("main") == 1)
                    harness.profiles({{"main", main}, {"low", profile(160)}});
            });
        harness.worker.start();
        const auto raw = sample();
        harness.push(raw);
        QTRY_COMPARE(harness.count("low"), 1);
        QCOMPARE(harness.layerPackets("low").first().size, QSize(160, 90));
        QVERIFY(harness.layerPackets("low").first().keyFrame);
        harness.push(raw);
        QTRY_COMPARE(harness.count("main"), 2);
        QVERIFY(!harness.layerPackets("main").last().keyFrame);
        QVERIFY(harness.errors.isEmpty());
    }

    void keyframeRequestAndCadenceAreIndependent() {
        Harness harness;
        harness.profiles({{"main", profile(640, 40)}, {"low", profile(320, 5)}});
        const auto raw = sample();
        QTimer producer;
        producer.setInterval(5);
        QObject::connect(&producer, &QTimer::timeout, &harness.owner, [&] { harness.push(raw); });
        harness.worker.start();
        producer.start();
        QTest::qWait(1100);
        producer.stop();
        harness.pause(true);
        QTest::qWait(40);
        QVERIFY2(harness.count("main") >= 12, "Main must not inherit low's five-fps cadence");
        QVERIFY(harness.count("low") >= 3);
        QVERIFY(harness.count("main") > harness.count("low") * 2);
        QVERIFY(harness.count("low") <= 7);
        const int previousMain = harness.count("main"), previousLow = harness.count("low");
        {
            QMutexLocker lock(&harness.state->mutex);
            harness.state->layers["low"].forceKeyFrame = true;
        }
        harness.pause(false);
        harness.push(raw);
        QTRY_VERIFY(harness.count("main") > previousMain);
        QTRY_VERIFY(harness.count("low") > previousLow);
        QVERIFY(!harness.layerPackets("main").last().keyFrame);
        QVERIFY(harness.layerPackets("low").last().keyFrame);
        QVERIFY(harness.errors.isEmpty());
    }

    void staticSurfaceUsesEachLayersOwnIdleInterval() {
        Harness harness;
        auto main = profile(640), low = profile(320);
        main.idleIntervalMs = 250;
        low.idleIntervalMs = 1000;
        harness.profiles({{"main", main}, {"low", low}});
        harness.worker.start();
        harness.push(sample());
        QTRY_COMPARE(harness.packets.size(), 2);
        QTest::qWait(1100);
        harness.pause(true);
        QTest::qWait(40);
        QVERIFY(harness.count("main") >= 4);
        QVERIFY(harness.count("main") <= 6);
        QCOMPARE(harness.count("low"), 2);
        QVERIFY(!harness.layerPackets("main").last().keyFrame);
        QVERIFY(!harness.layerPackets("low").last().keyFrame);
        QVERIFY(harness.errors.isEmpty());
    }

    void blockedGuiAndBackpressureKeepOneDeliveryAndLatestRawOnly() {
        Harness harness;
        harness.profiles({{"main", profile(640)}});
        const auto raw = sample();
        harness.worker.start();
        harness.push(raw);
        QElapsedTimer timeout;
        timeout.start();
        bool pending = false;
        while (!pending && timeout.elapsed() < 2000) {
            { QMutexLocker lock(&harness.state->mutex); pending = harness.state->deliveryPending; }
            if (!pending) QTest::qSleep(1);
        }
        QVERIFY(pending);
        for (int i = 0; i < 1000; ++i) harness.push(raw);
        harness.pause(true);
        QTest::qSleep(150); // Deliberately do not process the GUI callback yet.
        QCOMPARE(harness.packets.size(), 0);
        QTRY_COMPARE(harness.packets.size(), 1);
        QTest::qWait(100);
        QCOMPARE(harness.packets.size(), 1);
        harness.pause(false);
        QTRY_COMPARE(harness.packets.size(), 2);
        QVERIFY(!harness.packets.last().keyFrame);
        QTest::qWait(100);
        QCOMPARE(harness.packets.size(), 2);
        harness.pause(true);
        timeout.restart();
        harness.close();
        QVERIFY(timeout.elapsed() < 1000);
    }

    void failedLowEncoderDoesNotStopMain() {
        Harness harness;
        const auto main = profile(640, 1);
        harness.profiles({{"main", main}});
        QSignalSpy globalErrors(&harness.owner, &ScreenCaptureSource::errorOccurred);
        harness.worker.start();
        const auto raw = sample();
        harness.push(raw);
        QTRY_COMPARE(harness.count("main"), 1);
        harness.pause(true);
        harness.profiles({{"main", main}, {"low", profile(320)}});
        // Main's next cadence is still in the future, so only low sees the
        // unsupported frame and fails. Capture itself remains alive.
        const QVideoFrame unsupported(QVideoFrameFormat(QSize(640, 360), QVideoFrameFormat::Format_Y8));
        QVERIFY(unsupported.isValid());
        harness.push(unsupported);
        harness.pause(false);
        QTRY_COMPARE(harness.errors.size(), 1);
        QCOMPARE(globalErrors.size(), 0);
        harness.push(raw);
        QTRY_COMPARE(harness.count("main"), 2);
        QCOMPARE(harness.count("low"), 0);
        QVERIFY(!harness.layerPackets("main").last().keyFrame);
        QCOMPARE(harness.errors.size(), 1);
    }

    void emptyProfilesAndStopFenceQueuedPackets() {
        Harness harness;
        harness.profiles({{"main", profile(640)}, {"low", profile(320)}});
        harness.worker.start();
        harness.push(sample());
        QTest::qSleep(100);
        harness.profiles({});
        QCoreApplication::processEvents();
        QCOMPARE(harness.packets.size(), 0);
        harness.profiles({{"low", profile(320)}});
        QTRY_COMPARE(harness.count("low"), 1);
        harness.push(sample());
        QTest::qSleep(100);
        harness.close();
        QCoreApplication::processEvents();
        QCOMPARE(harness.count("low"), 1);
        QCOMPARE(harness.count("main"), 0);
    }

    void captureDiscontinuityCannotLeaveAFencedErrorPermanentlyPaused() {
        Harness harness;
        harness.profiles({{"low", profile(320)}});
        harness.worker.start();
        harness.push(QVideoFrame(QVideoFrameFormat(QSize(640, 360), QVideoFrameFormat::Format_Y8)));
        QElapsedTimer timeout;
        timeout.start();
        bool failed = false;
        while (!failed && timeout.elapsed() < 2000) {
            { QMutexLocker lock(&harness.state->mutex); failed = harness.state->layers.value("low").failed; }
            if (!failed) QTest::qSleep(1);
        }
        QVERIFY(failed);
        // Native suspension arrives before the GUI consumes the error. Both
        // stale data/error delivery and the old failed state must be fenced.
        harness.push({});
        harness.push(sample());
        QTRY_COMPARE(harness.count("low"), 1);
        QVERIFY(harness.errors.isEmpty());
        QVERIFY(harness.packets.first().keyFrame);
    }
};

QTEST_MAIN(ScreenCaptureLayersTest)
#include "tst_ScreenCaptureLayers.moc"

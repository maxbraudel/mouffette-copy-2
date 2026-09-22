#include <QtTest>
#include "backend/audiosharing/AudioPlaybackBuffer.h"
#include "backend/audiosharing/AudioPlaybackTimeline.h"
#include "backend/network/AudioPlayoutPolicy.h"
#include "backend/screensharing/ScreenAudioClock.h"
#include "backend/screensharing/ScreenPresentationQueue.h"
#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace {
constexpr qint64 Source = 900000000;
constexpr qint64 Local = 1000000;
using Pcm = std::array<float, AudioPlaybackBuffer::Frames * AudioPlaybackBuffer::Channels>;
Pcm constant(float left, float right) {
    Pcm samples{};
    for (int frame = 0; frame < AudioPlaybackBuffer::Frames; ++frame) {
        samples[frame * 2] = left;
        samples[frame * 2 + 1] = right;
    }
    return samples;
}
float maximumJump(const std::vector<float>& samples, int channel) {
    float previous = 0, maximum = 0;
    for (size_t frame = 0; frame < samples.size() / 2; ++frame) {
        const auto value = samples[frame * 2 + channel];
        maximum = std::max(maximum, std::abs(value - previous));
        previous = value;
    }
    return maximum;
}
}

class AudioPlaybackStabilityTest : public QObject {
    Q_OBJECT
private slots:
    void startupWaitsForContiguousRunway() {
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer;
        const auto pcm = constant(0.6f, -0.3f);
        std::array<float, 960> output{};
        QVERIFY(queue.push(pcm.data(), Source, Local));
        auto clock = renderer.render(queue, output.data(), 480, 2, 48000, Local);
        QCOMPARE(clock.sourceUs, qint64(-1));
        QVERIFY(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }));
        // Thirty milliseconds remaining is still insufficient. A third packet
        // gives a stable runway without moving its assigned presentation time.
        QVERIFY(queue.push(pcm.data(), Source + 20000, Local + 20000));
        clock = renderer.render(queue, output.data(), 480, 2, 48000, Local + 10000);
        QCOMPARE(clock.sourceUs, qint64(-1));
        QVERIFY(queue.push(pcm.data(), Source + 40000, Local + 40000));
        output.fill(0);
        clock = renderer.render(queue, output.data(), 480, 2, 48000, Local + 20000);
        QVERIFY(clock.sourceUs >= Source + 20000);
        QCOMPARE(clock.localUs - Local, clock.sourceUs - Source);
        QVERIFY(std::abs(output[958] - 0.6f) < 0.0001f);
        QVERIFY(std::abs(output[959] + 0.3f) < 0.0001f);
        QCOMPARE(renderer.takeUnderruns(), 0);
    }
    void timestampGapIsNotMistakenForContiguousRunway() {
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer;
        const auto pcm = constant(0.6f, -0.3f);
        std::array<float, 960> output{};
        QVERIFY(queue.push(pcm.data(), Source, Local));
        QVERIFY(queue.push(pcm.data(), Source + 60000, Local + 60000));
        const auto clock = renderer.render(queue, output.data(), 480, 2, 48000, Local);
        QCOMPARE(clock.sourceUs, qint64(-1));
        QVERIFY(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }));
    }
    void isolatedPacketsStaySilentAfterAnUnderrun() {
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer;
        const auto pcm = constant(0.6f, -0.3f);
        QVERIFY(queue.push(pcm.data(), Source, Local));
        QVERIFY(queue.push(pcm.data(), Source + 20000, Local + 20000));
        std::array<float, 960> output{};
        for (int callback = 0; callback < 8; ++callback) {
            output.fill(0);
            renderer.render(queue, output.data(), 480, 2, 48000, Local + callback * 10000);
        }
        QCOMPARE(renderer.takeUnderruns(), 1);
        for (int packet = 0; packet < 5; ++packet) {
            const auto offset = 100000 + packet * 40000;
            QVERIFY(queue.push(pcm.data(), Source + offset, Local + offset));
            for (int callback = 0; callback < 4; ++callback) {
                output.fill(0);
                const auto clock = renderer.render(queue, output.data(), 480, 2, 48000,
                    Local + offset + callback * 10000);
                QCOMPARE(clock.sourceUs, qint64(-1));
                QVERIFY(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }));
            }
        }
        QCOMPARE(renderer.takeUnderruns(), 0); // No repeated audible start/stop bursts.
    }
    void lossAndRecoveryHaveSmoothStereoEnvelopes_data() {
        QTest::addColumn<int>("rate");
        QTest::newRow("8k-fallback") << 8000;
        QTest::newRow("44k1") << 44100;
        QTest::newRow("48k") << 48000;
        QTest::newRow("96k") << 96000;
    }
    void lossAndRecoveryHaveSmoothStereoEnvelopes() {
        QFETCH(int, rate);
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer(rate);
        const auto first = constant(0.8f, -0.4f), second = constant(-0.6f, 0.7f);
        QVERIFY(queue.push(first.data(), Source, Local));
        QVERIFY(queue.push(first.data(), Source + 20000, Local + 20000));
        const int frames = rate / 100;
        std::vector<float> rendered, output(size_t(frames) * 2);
        for (int callback = 0; callback < 14; ++callback) {
            if (callback == 8) {
                QVERIFY(queue.push(second.data(), Source + 80000, Local + 80000, true));
                QVERIFY(queue.push(second.data(), Source + 100000, Local + 100000));
            }
            std::fill(output.begin(), output.end(), 0);
            renderer.render(queue, output.data(), frames, 2, rate, Local + callback * 10000);
            rendered.insert(rendered.end(), output.begin(), output.end());
        }
        for (int channel = 0; channel < 2; ++channel) {
            // A raised-cosine fade has max derivative amplitude*pi/(2*N).
            QVERIFY(maximumJump(rendered, channel) < float(0.8 * 3.141593 / (rate * 0.005)));
            QVERIFY(std::all_of(rendered.begin(), rendered.end(), [](float value) { return std::isfinite(value); }));
        }
        QVERIFY(std::abs(rendered[size_t(2 * rate / 100) * 2] - 0.8f) < 0.0001f);
        QVERIFY(std::abs(rendered[size_t(2 * rate / 100) * 2 + 1] + 0.4f) < 0.0001f);
        QVERIFY(std::abs(rendered[size_t(10 * rate / 100) * 2] + 0.6f) < 0.0001f);
        QVERIFY(std::abs(rendered[size_t(10 * rate / 100) * 2 + 1] - 0.7f) < 0.0001f);
        QCOMPARE(renderer.takeUnderruns(), 2);
    }
    void deadlineChangesKeepSmoothTransitions_data() {
        QTest::addColumn<qint64>("changeUs");
        QTest::newRow("increase-jitter-target") << qint64(50000);
        QTest::newRow("decrease-jitter-target") << qint64(-50000);
    }
    void deadlineChangesKeepSmoothTransitions() {
        QFETCH(qint64, changeUs);
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer;
        const auto first = constant(0.7f, -0.35f), next = constant(-0.6f, 0.3f);
        for (int packet = 0; packet < AudioPlaybackBuffer::Capacity; ++packet) {
            const auto& pcm = packet < 2 ? first : next;
            QVERIFY(queue.push(pcm.data(), Source + packet * 20000,
                Local + packet * 20000 + (packet < 2 ? 0 : changeUs)));
        }
        std::array<float, 960> output{};
        std::vector<float> rendered;
        bool playedNewDeadline = false;
        for (int callback = 0; callback < 24; ++callback) {
            output.fill(0);
            const auto clock = renderer.render(queue, output.data(), 480, 2, 48000,
                Local + callback * 10000);
            rendered.insert(rendered.end(), output.begin(), output.end());
            if (clock.sourceUs >= Source + 40000) {
                playedNewDeadline = true;
                QVERIFY(std::abs(clock.localUs - Local - (clock.sourceUs - Source) - changeUs) < 1000);
            }
        }
        QVERIFY(playedNewDeadline);
        QVERIFY(maximumJump(rendered, 0) < 0.01f);
        QVERIFY(maximumJump(rendered, 1) < 0.01f);
        QVERIFY(renderer.takeRebuffers() > 0);
    }
    void overflowDoesNotReleaseConsumerPointersUntilAcknowledged() {
        AudioPlaybackBuffer queue;
        const auto first = constant(0.25f, -0.5f), next = constant(-0.7f, 0.1f);
        for (int packet = 0; packet < AudioPlaybackBuffer::Capacity; ++packet)
            QVERIFY(queue.push(first.data(), Source + packet * 20000, Local + packet * 20000));
        const auto* held = queue.peek();
        QVERIFY(held);
        queue.requestDiscardQueued();
        // Merely publishing a request cannot permit the producer to overwrite
        // the pointer held by an in-progress device callback.
        QVERIFY(!queue.push(next.data(), Source + 160000, Local + 160000));
        QCOMPARE(queue.size(), size_t(AudioPlaybackBuffer::Capacity));
        QCOMPARE(queue.peek(), held);
        QCOMPARE(held->pcm, first);
        QVERIFY(queue.applyDiscardRequest());
        QVERIFY(!queue.applyDiscardRequest());
        QVERIFY(queue.push(next.data(), Source + 180000, Local + 180000, true));
        QCOMPARE(queue.peek()->pcm, next);
        QVERIFY(queue.peek()->discontinuity);
    }
    void overflowFadesOutAndResumesAtTheFreshDeadline() {
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer;
        const auto oldPcm = constant(0.7f, -0.35f), newPcm = constant(-0.5f, 0.25f);
        QVERIFY(queue.push(oldPcm.data(), Source, Local));
        QVERIFY(queue.push(oldPcm.data(), Source + 20000, Local + 20000));
        std::array<float, 960> output{};
        std::vector<float> rendered;
        renderer.render(queue, output.data(), 480, 2, 48000, Local);
        rendered.insert(rendered.end(), output.begin(), output.end());
        queue.requestDiscardQueued();
        output.fill(0);
        auto clock = renderer.render(queue, output.data(), 480, 2, 48000, Local + 10000);
        rendered.insert(rendered.end(), output.begin(), output.end());
        QCOMPARE(clock.sourceUs, qint64(-1));
        QCOMPARE(output[958], 0.0f);
        QCOMPARE(output[959], 0.0f);
        QVERIFY(queue.push(newPcm.data(), Source + 40000, Local + 40000, true));
        QVERIFY(queue.push(newPcm.data(), Source + 60000, Local + 60000));
        for (int callback = 2; callback < 6; ++callback) {
            output.fill(0);
            clock = renderer.render(queue, output.data(), 480, 2, 48000, Local + callback * 10000);
            rendered.insert(rendered.end(), output.begin(), output.end());
            if (callback < 4) QCOMPARE(clock.sourceUs, qint64(-1));
            else QVERIFY(clock.sourceUs >= Source + 40000);
        }
        QVERIFY(maximumJump(rendered, 0) < 0.01f);
        QVERIFY(maximumJump(rendered, 1) < 0.01f);
        QVERIFY(renderer.takeRebuffers() >= 1);
    }
    void discontinuityDoesNotCountAsRecoveryLookahead() {
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer;
        const auto pcm = constant(0.5f, -0.25f);
        QVERIFY(queue.push(pcm.data(), Source, Local));
        QVERIFY(queue.push(pcm.data(), Source + 20000, Local + 20000, true));
        std::array<float, 960> output{};
        QCOMPARE(renderer.render(queue, output.data(), 480, 2, 48000, Local).sourceUs, qint64(-1));
        QVERIFY(queue.push(pcm.data(), Source + 40000, Local + 40000));
        const auto clock = renderer.render(queue, output.data(), 480, 2, 48000, Local + 20000);
        QVERIFY(clock.sourceUs >= Source + 20000);
    }
    void deviceQuantumBudgetIsBoundedAndSurvivesStreamReset() {
        AudioPlayoutPolicy policy;
        QCOMPARE(policy.targetDelayUs(), qint64(120000));
        policy.setOutputQuantumUs(40000);
        QCOMPARE(policy.targetDelayUs(), qint64(160000));
        policy.setOutputQuantumUs(80000);
        QCOMPARE(policy.targetDelayUs(), qint64(240000));
        policy.observe(Source, Local + 86500, Local);
        QCOMPARE(policy.targetDelayUs(), qint64(266500));
        policy.reset();
        QCOMPARE(policy.outputQuantumUs(), qint64(80000));
        QCOMPARE(policy.targetDelayUs(), qint64(240000));
        policy.setOutputQuantumUs(5000000); // A malformed hint cannot unbound queues.
        QCOMPARE(policy.outputQuantumUs(), qint64(100000));
        QCOMPARE(policy.targetDelayUs(), qint64(280000));
        policy.observe(Source, Local + 150000, Local);
        QCOMPARE(policy.targetDelayUs(), qint64(310000));
        policy.setOutputQuantumUs(20000);
        QCOMPARE(policy.targetDelayUs(), qint64(150000));
        policy.reset();
        QCOMPARE(policy.targetDelayUs(), qint64(120000));
        QVERIFY(qint64(AudioPlaybackBuffer::Capacity) * 20000 > AudioOutputTiming::MaximumPlayoutDelayUs);
    }
    void freshButUnplayablePathsReportAStableTimingFailure_data() {
        QTest::addColumn<qint64>("quantumUs");
        QTest::newRow("usual20ms") << qint64(20000);
        QTest::newRow("large80ms") << qint64(80000);
    }
    void freshButUnplayablePathsReportAStableTimingFailure() {
        QFETCH(qint64, quantumUs);
        AudioPlayoutPolicy policy;
        policy.setOutputQuantumUs(quantumUs);
        for (qint64 packet = 0; packet < 100; ++packet) {
            const auto mapped = Local + packet * 20000;
            QVERIFY(!policy.observe(Source + packet * 20000, mapped + 86500, mapped));
        }
        policy.reset();
        for (qint64 packet = 0; packet < 50; ++packet) {
            const auto mapped = Local + packet * 20000;
            QVERIFY(!policy.observe(Source + packet * 20000, mapped + 120000, mapped));
        }
        QVERIFY(policy.observe(Source + 1000000, Local + 1120000, Local + 1000000));
        QVERIFY(policy.targetDelayUs() <= AudioOutputTiming::maximumPlayoutDelayUs(quantumUs));
        // A healthy packet clears sustained pressure, even after a failure.
        QVERIFY(!policy.observe(Source + 1060000, Local + 1146500, Local + 1060000));
        QVERIFY(!policy.observe(Source + 1080000, Local + 1200000, Local + 1080000));
    }
    void intermittentJitterAndSingleBurstsDoNotFailTheRoute() {
        AudioPlayoutPolicy policy;
        qint64 lastArrival = 0;
        for (qint64 packet = 0; packet < 500; ++packet) {
            const auto mapped = Local + packet * 20000;
            const auto lag = packet % 25 >= 23 ? 86500 : 120000;
            const auto arrival = std::max(lastArrival, mapped + lag);
            QVERIFY(!policy.observe(Source + packet * 20000, arrival, mapped));
            lastArrival = arrival;
        }
        policy.reset();
        for (qint64 packet = 0; packet < 100; ++packet)
            QVERIFY(!policy.observe(Source + packet * 20000, Local + 5000000, Local + packet * 20000));
    }
    void sustainedBatchedPacketsCannotHideAnUnplayablePath() {
        AudioPlayoutPolicy policy;
        policy.setOutputQuantumUs(80000);
        for (qint64 batch = 0; batch < 25; ++batch) {
            const auto mapped = Local + batch * 40000;
            const auto source = Source + batch * 40000;
            QVERIFY(!policy.observe(source, mapped + 140000, mapped));
            QVERIFY(!policy.observe(source + 20000, mapped + 140000, mapped + 20000));
        }
        QVERIFY(policy.observe(Source + 1000000, Local + 1140000, Local + 1000000));
    }
    void timelineAcceptsOnlyTheActiveDeviceBudget() {
        AudioPlaybackTimeline timeline;
        QVERIFY(!timeline.enqueue(Source, Local, false, Local + 240000).accept);
        timeline.setOutputQuantumUs(80000);
        QVERIFY(timeline.enqueue(Source, Local, false, Local + 240000).accept);
        QVERIFY(!timeline.enqueue(Source + 20000, Local + 20000, false, Local + 290001).accept);
        timeline.setOutputQuantumUs(20000);
        QCOMPARE(timeline.presentationAt(Source), Local + 120000);
        QVERIFY(!timeline.enqueue(Source + 40000, Local + 40000, false, Local + 240000).accept);
        timeline.reset();
        QVERIFY(timeline.enqueue(Source, Local).accept);
        timeline.setOutputQuantumUs(80000);
        QCOMPARE(timeline.presentationAt(Source), Local + 200000);
    }
    void largeDeviceCallbacksHaveContinuousStereo_data() {
        QTest::addColumn<int>("rate");
        QTest::addColumn<qint64>("quantumUs");
        for (const int rate : {44100, 48000})
            for (const qint64 quantum : {40000, 80000, 100000})
                QTest::newRow(qPrintable(QStringLiteral("%1Hz-%2ms").arg(rate).arg(quantum / 1000))) << rate << quantum;
    }
    void largeDeviceCallbacksHaveContinuousStereo() {
        QFETCH(int, rate);
        QFETCH(qint64, quantumUs);
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer(rate);
        AudioPlaybackTimeline timeline;
        AudioPlayoutPolicy policy;
        policy.setOutputQuantumUs(quantumUs);
        timeline.setOutputQuantumUs(quantumUs);
        const auto pcm = constant(0.45f, -0.225f);
        const int frames = int(quantumUs * rate / 1000000);
        std::vector<float> output(size_t(frames) * 2), rendered;
        int packet = 0, activeCallbacks = 0;
        bool started = false;
        for (qint64 elapsed = 0; elapsed < 4000000; elapsed += 1000) {
            // Include bounded network jitter and real source collection age.
            while (qint64(packet) * 20000 + 86500 + (packet % 4) * 2000 <= elapsed) {
                const auto source = Source + qint64(packet) * 20000;
                const auto mapped = Local + qint64(packet) * 20000;
                QVERIFY(!policy.observe(source, Local + elapsed, mapped));
                const auto deadline = mapped + policy.targetDelayUs();
                const auto decision = timeline.enqueue(source, Local + elapsed, false, deadline);
                QVERIFY(decision.accept);
                QVERIFY(queue.push(pcm.data(), source, deadline, decision.rebuffer));
                ++packet;
            }
            if (elapsed % quantumUs) continue;
            std::fill(output.begin(), output.end(), 0);
            // Match Engine's fixed scratch chunks while retaining the full
            // outer device quantum for recovery and presentation timing.
            bool audible = false;
            for (int offset = 0; offset < frames; offset += 1024) {
                const int count = std::min(1024, frames - offset);
                const auto clock = renderer.render(queue, output.data() + offset * 2, count, 2, rate,
                    Local + elapsed + quantumUs + qint64(offset) * 1000000 / rate, quantumUs);
                audible |= clock.sourceUs >= 0;
                if (clock.sourceUs >= 0)
                    QVERIFY(std::abs(clock.localUs - Local - (clock.sourceUs - Source) - policy.targetDelayUs()) < 10000);
            }
            rendered.insert(rendered.end(), output.begin(), output.end());
            if (audible) { ++activeCallbacks; started = true; }
            else QVERIFY(!started); // A stable source must never starve after activation.
        }
        QVERIFY(activeCallbacks > 3000000 / quantumUs);
        QCOMPARE(renderer.takeUnderruns(), 0);
        QVERIFY(maximumJump(rendered, 0) < 0.01f);
        for (size_t sample = 0; sample < rendered.size(); sample += 2)
            QCOMPARE(rendered[sample + 1], -0.5f * rendered[sample]);
    }
    void deviceQuantumChangesRecoverWithoutHardSteps_data() {
        QTest::addColumn<int>("rate");
        QTest::newRow("44k1") << 44100;
        QTest::newRow("48k") << 48000;
    }
    void deviceQuantumChangesRecoverWithoutHardSteps() {
        QFETCH(int, rate);
        AudioPlaybackBuffer queue;
        AudioPlaybackRenderer renderer(rate);
        AudioPlaybackTimeline timeline;
        AudioPlayoutPolicy policy;
        const auto pcm = constant(0.5f, -0.25f);
        int packet = 0, audibleAfterChange = 0;
        qint64 quantumUs = 20000, nextCallback = 0;
        std::vector<float> rendered;
        for (qint64 elapsed = 0; elapsed < 4000000; elapsed += 1000) {
            if (elapsed == 1000000 || elapsed == 2200000) {
                quantumUs = elapsed == 1000000 ? 80000 : 40000;
                policy.setOutputQuantumUs(quantumUs);
                timeline.setOutputQuantumUs(quantumUs);
            }
            while (qint64(packet) * 20000 + 86500 <= elapsed) {
                const auto source = Source + qint64(packet) * 20000;
                const auto mapped = Local + qint64(packet) * 20000;
                policy.observe(source, Local + elapsed, mapped);
                const auto deadline = mapped + policy.targetDelayUs();
                const auto decision = timeline.enqueue(source, Local + elapsed, false, deadline);
                QVERIFY(decision.accept);
                QVERIFY(queue.push(pcm.data(), source, deadline, decision.rebuffer));
                ++packet;
            }
            if (elapsed < nextCallback) continue;
            nextCallback = elapsed + quantumUs;
            const int frames = int(quantumUs * rate / 1000000);
            std::vector<float> output(size_t(frames) * 2);
            bool audible = false;
            for (int offset = 0; offset < frames; offset += 1024) {
                const int count = std::min(1024, frames - offset);
                const auto clock = renderer.render(queue, output.data() + offset * 2, count, 2, rate,
                    Local + elapsed + quantumUs + qint64(offset) * 1000000 / rate, quantumUs);
                audible |= clock.sourceUs >= 0;
            }
            rendered.insert(rendered.end(), output.begin(), output.end());
            if ((elapsed > 1500000 && elapsed < 2200000) || elapsed > 2700000) {
                QVERIFY(audible);
                ++audibleAfterChange;
            }
        }
        QVERIFY(audibleAfterChange > 30);
        QVERIFY(renderer.takeUnderruns() <= 2); // One bounded interruption per device change is permitted.
        QVERIFY(maximumJump(rendered, 0) < 0.01f);
        QVERIFY(maximumJump(rendered, 1) < 0.01f);
        QVERIFY(renderer.takeRebuffers() > 0);
    }
    void largerDeviceClockKeepsVideoAtItsRealAudioDeadline() {
        ScreenAudioClock clock;
        ScreenPresentationQueue<int> frames;
        QCOMPARE(clock.maximumVideoWaitUs(), qint64(150000));
        clock.setOutputQuantumUs(80000);
        QCOMPARE(clock.maximumVideoWaitUs(), qint64(270000));
        QCOMPARE(clock.maximumClockLeadUs(), qint64(160000));
        // The end of an80ms callback may be160ms ahead when published. This
        // paired anchor must remain usable by video, and its240ms target must
        // not be cut off at the normal150ms screen queue limit.
        clock.update(Source, Local + 160000);
        QCOMPARE(clock.videoDelayUs(Source + 80000, Local), qint64(240000));
        frames.push(1, Source + 80000, Local, 4);
        QVERIFY(!frames.takeReady(clock, Local + 150000));
        QVERIFY(!frames.takeReady(clock, Local + 239999));
        QCOMPARE(*frames.takeReady(clock, Local + 240000), 1);
        clock.reset();
        QCOMPARE(clock.maximumVideoWaitUs(), qint64(270000));
        clock.setOutputQuantumUs(20000);
        QCOMPARE(clock.maximumVideoWaitUs(), qint64(150000));
        QCOMPARE(clock.maximumClockLeadUs(), qint64(100000));
    }
    void concurrentDiscardNeverPublishesTornPcm() {
        AudioPlaybackBuffer queue;
        std::atomic<bool> producerDone{false}, valid{true};
        constexpr int Packets = 10000;
        std::thread producer([&] {
            for (int packet = 0; packet < Packets; ++packet) {
                const auto pcm = constant(float(packet), -float(packet));
                if (!queue.push(pcm.data(), Source + packet * 20000, Local + packet * 20000))
                    queue.requestDiscardQueued();
            }
            producerDone.store(true, std::memory_order_release);
        });
        qint64 lastTimestamp = -1;
        while (!producerDone.load(std::memory_order_acquire) || queue.size()) {
            queue.applyDiscardRequest();
            const auto* block = queue.peek();
            if (!block) { std::this_thread::yield(); continue; }
            const float packet = float((block->sourceTimestampUs - Source) / 20000);
            if (block->sourceTimestampUs <= lastTimestamp) valid.store(false);
            lastTimestamp = block->sourceTimestampUs;
            for (int frame = 0; frame < AudioPlaybackBuffer::Frames; ++frame)
                if (block->pcm[frame * 2] != packet || block->pcm[frame * 2 + 1] != -packet)
                    valid.store(false);
            // Looking ahead cannot apply a concurrent drop request.
            queue.peek(1);
            if (block->pcm[0] != packet) valid.store(false);
            queue.pop();
        }
        producer.join();
        QVERIFY(valid.load());
    }
};

QTEST_GUILESS_MAIN(AudioPlaybackStabilityTest)
#include "tst_AudioPlaybackStability.moc"

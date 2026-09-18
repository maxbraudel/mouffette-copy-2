#include <QApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/scene/SceneTimeline.h"
#include "backend/media/MediaResidencyManager.h"
#include "frontend/qml/MediaSettingsViewModel.h"
#include "frontend/qml/TimelineController.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"

class TimelineControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void previewNeverSavesAndCaptureCommitsOnlyTheDraft()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* document = host->document();
        auto* media = document->addText({20, 30}, QStringLiteral("Before"));
        QVERIFY(media);
        media->setPosition({20, 30});
        document->select(media->mediaId());
        TimelineController timeline;
        timeline.setHost(host.get());
        timeline.placeKeyframe();
        QCOMPARE(media->timelineTrack().keyframes.size(), 1);
        timeline.seek(1000);
        MediaSettingsViewModel settings;
        settings.setController(host->controller());
        settings.setUppercase(true);
        media->beginElementEdit();
        media->setPosition({120, 80});
        QVERIFY(timeline.hasDraft());
        QCOMPARE(media->authorElementState().position, QPointF(20, 30));
        timeline.placeKeyframe();
        QCOMPARE(media->timelineTrack().keyframes.size(), 2);
        QVERIFY(!timeline.hasDraft());
        const auto saved = document->serializeProjectState();
        QSignalSpy writes(document, &CanvasDocument::documentChanged);
        timeline.seek(500);
        QCOMPARE(media->position(), QPointF(70, 55));
        QVERIFY(!media->uppercase());
        QCOMPARE(writes.count(), 0);
        QCOMPARE(document->serializeProjectState(), saved);
        settings.setItalic(true);
        QVERIFY(timeline.hasDraft());
        timeline.seek(1000);
        QVERIFY(!timeline.hasDraft());
        QVERIFY(!media->italic());
        QVERIFY(media->uppercase());
        QCOMPARE(writes.count(), 0);
        QCOMPARE(document->serializeProjectState(), saved);
    }

    void primarySelectionOwnsTransformsAndDiscardsPreviousDraft()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* a = host->document()->addText({10, 20}, QStringLiteral("A"));
        auto* b = host->document()->addText({300, 40}, QStringLiteral("B"));
        host->document()->select(a->mediaId());
        host->document()->select(b->mediaId(), true);
        QCOMPARE(host->controller()->primarySelectedMediaId(), b->mediaId());
        TimelineController timeline;
        timeline.setHost(host.get());
        timeline.placeKeyframe();
        const auto aRect = a->sceneRect();
        const auto bRect = b->sceneRect();
        host->controller()->scaleSelectionBy(2);
        QCOMPARE(a->sceneRect(), aRect);
        QCOMPARE(b->sceneRect().width(), bRect.width() * 2);
        QVERIFY(timeline.hasDraft());
        host->document()->select(a->mediaId());
        QCOMPARE(host->document()->selectedMediaIds().size(), 2);
        QCOMPARE(host->document()->primarySelectedMediaId(), a->mediaId());
        QCOMPARE(b->sceneRect(), bRect);
        QVERIFY(!timeline.hasDraft());
        int primaryCount = 0;
        for (const auto& row : host->controller()->selectionChromeModel())
            primaryCount += row.toMap().value(QStringLiteral("isPrimary")).toBool();
        QCOMPARE(primaryCount, 1);
    }

    void keyframeCopyCollisionAndFinalDeletionBecomeStatic()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* media = host->document()->addText({}, QStringLiteral("Key"));
        host->document()->select(media->mediaId());
        TimelineController timeline;
        timeline.setHost(host.get());
        timeline.seek(713);
        timeline.placeKeyframe();
        timeline.copySelected();
        timeline.seek(1429);
        QVERIFY(timeline.canPaste());
        timeline.paste();
        QCOMPARE(media->timelineTrack().keyframes.size(), 2);
        timeline.moveKeyframe(timeline.selectedKeyframeId(), 713);
        QCOMPARE(media->timelineTrack().keyframes.size(), 1);
        media->beginElementEdit();
        media->setText(QStringLiteral("Keep this draft"));
        timeline.deleteSelected();
        QVERIFY(media->timelineTrack().keyframes.isEmpty());
        QCOMPARE(media->text(), QStringLiteral("Keep this draft"));
        QVERIFY(!media->hasEvaluatedElementState());
        QVERIFY(!timeline.hasDraft());
    }

    void snappingUsesAllTracksAndBothClipEdges()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* a = host->document()->addText({}, QStringLiteral("A"));
        auto* b = host->document()->addText({}, QStringLiteral("B"));
        SceneTimeline::MediaTrack track;
        track.keyframes = {{QStringLiteral("target"), 1337, b->authorElementState()}};
        b->setTimelineTrack(track);
        host->document()->select(a->mediaId());
        TimelineController timeline;
        timeline.setHost(host.get());
        auto snap = timeline.snapTime(1327, 1, QString());
        QVERIFY(snap.value(QStringLiteral("snapped")).toBool());
        QCOMPARE(snap.value(QStringLiteral("timeMs")).toLongLong(), 1337);
        QVERIFY(!timeline.snapTime(1326, 1, QString()).value(QStringLiteral("snapped")).toBool());
        QVERIFY(!timeline.snapTime(1337, 1, QStringLiteral("target")).value(QStringLiteral("snapped")).toBool());
        snap = timeline.snapTime(1000, 1, QString(), 340);
        QCOMPARE(snap.value(QStringLiteral("timeMs")).toLongLong(), 997);
    }

    void clipboardCannotChangeTheOccurrenceType()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* media = host->document()->addText({}, QStringLiteral("Keep text"));
        host->document()->select(media->mediaId());
        TimelineController timeline;
        timeline.setHost(host.get());
        SceneTimeline::ElementState wrongType;
        wrongType.type = QStringLiteral("image");
        auto* mime = new QMimeData;
        mime->setData("application/x-mouffette-timeline-v3", QJsonDocument(QJsonObject{
            {"mediaId", media->mediaId()}, {"projectId", host->document()->projectId()},
            {"kind", "keyframe"}, {"state", wrongType.toJson()}}).toJson());
        QGuiApplication::clipboard()->setMimeData(mime);
        timeline.paste();
        QVERIFY(media->timelineTrack().keyframes.isEmpty());
        QVERIFY(!timeline.errorText().isEmpty());
        QCOMPARE(media->text(), QStringLiteral("Keep text"));
    }

    void clipPasteTruncatesAndOverwritesWithoutMovingKeys()
    {
        MediaResidencyManager::instance().setMemorySnapshotForTesting(
            {8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        SceneTimeline::SceneSettings timing;
        timing.maxDurationMs = 2500;
        QVERIFY(host->document()->setTimelineSettings(timing));
        auto* media = host->document()->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {160, 90}, true, {});
        QVERIFY(media);
        QTRY_VERIFY(media->residencyReady() && media->player()->duration() > 2500);
        host->document()->select(media->mediaId());
        SceneTimeline::MediaTrack track;
        track.clipsInitialized = true;
        track.clips = {{QStringLiteral("original"), 0, 0, 2000}};
        media->setTimelineTrack(track);
        TimelineController timeline;
        timeline.setHost(host.get());
        timeline.placeKeyframe();
        const auto key = media->timelineTrack().keyframes.first().id;
        timeline.selectClip(QStringLiteral("original"));
        timeline.copySelected();
        timeline.seek(1750);
        timeline.paste();
        QCOMPARE(media->timelineTrack().clips.size(), 2);
        QCOMPARE(media->timelineTrack().clips.first().sourceOutMs, 1750);
        QCOMPARE(media->timelineTrack().clips.last().durationMs(), 750);
        QCOMPARE(media->timelineTrack().clips.last().endMs(), 2500);
        QCOMPARE(media->timelineTrack().keyframes.first().id, key);
        QCOMPARE(media->timelineTrack().keyframes.first().timeMs, 0);
        host.reset();
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }

    void productionTimelineQmlLoadsAndCapturesAtFreeTime()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* media = host->document()->addText({}, QStringLiteral("Timeline UI"));
        host->document()->select(media->mediaId());
        TimelineController timeline;
        timeline.setHost(host.get());
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(1100, 240);
        view.setInitialProperties({{QStringLiteral("session"), QVariantMap{
            {QStringLiteral("timeline"), QVariant::fromValue<QObject*>(&timeline)}}}});
        view.setSource(QUrl(QStringLiteral("qrc:/qt/qml/Mouffette/App/resources/qml/app/canvas/TimelinePanel.qml")));
        QVERIFY2(view.status() == QQuickView::Ready, qPrintable(view.errors().isEmpty() ? QString() : view.errors().first().toString()));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        timeline.seek(1337);
        auto* capture = view.rootObject()->findChild<QQuickItem*>(QStringLiteral("timelinePlaceKeyframe"));
        QVERIFY(capture);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
            capture->mapToScene(QPointF(capture->width()/2, capture->height()/2)).toPoint());
        QTRY_COMPARE(media->timelineTrack().keyframes.size(), 1);
        QCOMPARE(media->timelineTrack().keyframes.first().timeMs, 1337);
        auto* timeField = view.rootObject()->findChild<QQuickItem*>(QStringLiteral("timelineTimeField"));
        QVERIFY(timeField);
        timeField->forceActiveFocus();
        QTRY_VERIFY(view.rootObject()->property("textInputFocused").toBool());
        QQuickItem* keyItem = nullptr;
        auto findKey = [&](auto&& find, QQuickItem* item) -> void {
            if (item->objectName() == QStringLiteral("timelineKeyframe")) keyItem = item;
            for (auto* child : item->childItems()) find(find, child);
        };
        findKey(findKey, view.rootObject());
        QVERIFY(keyItem);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
            keyItem->mapToScene(QPointF(keyItem->width()/2, keyItem->height()/2)).toPoint());
        QTRY_VERIFY(!view.rootObject()->property("textInputFocused").toBool());
        QCOMPARE(timeline.selectedKeyframeId(), media->timelineTrack().keyframes.first().id);
        QCOMPARE(host->document()->media().size(), 1);
        QVERIFY(view.rootObject()->findChild<QQuickItem*>(QStringLiteral("timelinePlayPause")));
        QVERIFY(!view.rootObject()->findChild<QQuickItem*>(QStringLiteral("videoProgressSlider")));
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    TimelineControllerTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_TimelineController.moc"

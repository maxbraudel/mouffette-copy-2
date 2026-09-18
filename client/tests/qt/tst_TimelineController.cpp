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

namespace {
QList<QQuickItem*> timelineItems(QQuickItem* root, const QString& name)
{
    QList<QQuickItem*> items;
    if (root->objectName() == name) items.append(root);
    for (auto* child : root->childItems()) items.append(timelineItems(child, name));
    return items;
}

struct TimelineFixture {
    std::unique_ptr<QuickCanvasHost> host{QuickCanvasHost::create()};
    TimelineController timeline;
    QQuickView view;
    bool initialize()
    {
        if (!host) return false;
        host->setProjectEditingEnabled(true);
        timeline.setHost(host.get());
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(1100, 240);
        view.setInitialProperties({{"session", QVariantMap{{"timeline", QVariant::fromValue<QObject*>(&timeline)}}}});
        view.setSource(QUrl("qrc:/qt/qml/Mouffette/App/resources/qml/app/canvas/TimelinePanel.qml"));
        if (view.status() != QQuickView::Ready) return false;
        view.show();
        return QTest::qWaitForWindowExposed(&view);
    }
    QQuickItem* item(const QString& name) const { return timelineItems(view.rootObject(), name).value(0); }
    qreal scroll() const { return item("timelineTracks")->property("contentX").toReal(); }
    qreal scale() const { return view.rootObject()->property("pixelsPerMs").toReal(); }
    qreal timeAt(qreal x) const { return (scroll() + x - 12) / scale(); }
    void wheel(const QPoint& point, const QPoint& pixels, const QPoint& angles,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool inverted = false)
    {
        QWheelEvent event(point, view.mapToGlobal(point), pixels, angles, Qt::NoButton,
            modifiers, Qt::NoScrollPhase, inverted);
        QCoreApplication::sendEvent(&view, &event);
    }
};
}

class TimelineControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void rulerDragScrubsAndBothWheelAxesScroll()
    {
        TimelineFixture f;
        QVERIFY(f.initialize());
        auto* tracks = f.item("timelineTracks");
        tracks->setProperty("contentX", 500.0);
        const auto saved = f.host->serializeProjectState();
        const QPoint from = tracks->mapToScene({220, 10}).toPoint();
        const QPoint to = from + QPoint(260, 0);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(&f.view, to, 20);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(f.scroll(), 500.0);
        QCOMPARE(f.timeline.positionMs(), f.timeline.gridTime(f.timeAt(480)));
        const auto position = f.timeline.positionMs();
        const auto scale = f.scale();
        f.wheel(to, {}, {0, -120});
        QCOMPARE(f.scroll(), 545.0);
        f.wheel(to, {-40, 0}, {});
        QCOMPARE(f.scroll(), 585.0);
        f.wheel(to, {0, -40}, {});
        QCOMPARE(f.scroll(), 625.0);
        f.wheel(to, {40, 0}, {});
        f.wheel(to, {0, 40}, {});
        f.wheel(to, {}, {0, 120});
        QCOMPARE(f.scroll(), 500.0);
        QCOMPARE(f.timeline.positionMs(), position);
        QCOMPARE(f.scale(), scale);
        QCOMPARE(f.host->serializeProjectState(), saved);
    }

    void zoomAnchorsButtonsToHeadAndWheelAndShortcutsToPointer()
    {
        TimelineFixture f;
        QVERIFY(f.initialize());
        auto* root = f.view.rootObject();
        auto* tracks = f.item("timelineTracks");
        tracks->setProperty("contentX", 350.0);
        f.timeline.seek(10000);
        const qreal headX = 12 + f.timeline.positionMs() * f.scale() - f.scroll();
        const auto click = [&](const QString& name) {
            auto* button = f.item(name);
            QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene({button->width()/2, button->height()/2}).toPoint());
        };
        click("timelineZoomIn");
        QVERIFY(qAbs(f.timeAt(headX) - f.timeline.positionMs()) < 1e-6);
        click("timelineZoomOut");
        QVERIFY(qAbs(f.timeAt(headX) - f.timeline.positionMs()) < 1e-6);
        // A head outside the viewport must be brought back into view by the buttons.
        f.timeline.seek(90000);
        click("timelineZoomIn");
        QVERIFY(qAbs(f.timeAt(tracks->width()/2) - f.timeline.positionMs()) < 1e-6);
        const qreal cursorX = qRound(tracks->width() * 0.7);
        const QPoint cursor = tracks->mapToScene({cursorX, 12}).toPoint();
        for (auto modifier : {Qt::ControlModifier, Qt::MetaModifier}) {
            for (bool pixels : {false, true}) {
                for (bool natural : {false, true}) {
                    const qreal anchor = f.timeAt(cursorX);
                    const qreal scale = f.scale();
                    const int direction = natural ? -1 : 1;
                    f.wheel(cursor, pixels ? QPoint(0, direction * 24) : QPoint(),
                        {0, direction * 120}, modifier, natural);
                    QVERIFY(f.scale() > scale);
                    QVERIFY(qAbs(f.timeAt(cursorX) - anchor) < 1e-6);
                    f.wheel(cursor, pixels ? QPoint(0, -direction * 24) : QPoint(),
                        {0, -direction * 120}, modifier, natural);
                    QVERIFY(qAbs(f.scale() - scale) < 1e-6);
                    QVERIFY(qAbs(f.timeAt(cursorX) - anchor) < 1e-6);
                }
            }
        }
        f.view.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&f.view));
        root->forceActiveFocus();
        QTest::mouseMove(&f.view, cursor);
        const qreal anchor = f.timeAt(cursorX);
        const qreal scale = f.scale();
        QTest::keyClick(&f.view, Qt::Key_Equal, Qt::ControlModifier);
        QTRY_VERIFY(f.scale() > scale);
        QVERIFY(qAbs(f.timeAt(cursorX) - anchor) < 1e-6);
        QTest::keyClick(&f.view, Qt::Key_Minus, Qt::ControlModifier);
        QTRY_VERIFY(qAbs(f.scale() - scale) < 1e-6);
        QVERIFY(qAbs(f.timeAt(cursorX) - anchor) < 1e-6);
    }

    void backgroundMediaStayCenteredVisibleAndCannotCollide()
    {
        auto& residency = MediaResidencyManager::instance();
        residency.setMemorySnapshotForTesting({8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
        const auto resetMemory = qScopeGuard([&] { residency.clearMemorySnapshotForTesting(); });
        TimelineFixture f;
        QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* primary = doc->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {160,90}, true, {});
        auto* other = doc->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {160,90}, true, {});
        QVERIFY(primary && other);
        QTRY_VERIFY(primary->residencyReady() && other->residencyReady()
                    && primary->sourceDurationMs() > 5000 && other->sourceDurationMs() > 5000);
        SceneTimeline::MediaTrack track;
        track.clipsInitialized = true;
        track.clips = {{"moving", 0, 0, 30}, {"stationary", 60, 30, 30}};
        track.keyframes = {{"primary-key", 30, primary->authorElementState()}};
        primary->setTimelineTrack(track);
        track.clips = {{"background", 120, 0, 30}};
        track.keyframes = {{"background-key", 60, other->authorElementState()}};
        other->setTimelineTrack(track);
        doc->select(primary->mediaId());
        QCOMPARE(f.timeline.otherClips().size(), 1);
        auto* keyTrack = f.item("timelineKeyframeTrack");
        auto* foreground = f.item("timelineKeyframeDiamond");
        auto* background = f.item("otherMediaKeyframe");
        QVERIFY(foreground && background);
        QCOMPARE(foreground->size(), background->size());
        QCOMPARE(foreground->rotation(), background->rotation());
        const auto keyCenter = keyTrack->mapToScene({0, keyTrack->height()/2}).y();
        QCOMPARE(foreground->mapToScene({foreground->width()/2, foreground->height()/2}).y(), keyCenter);
        QCOMPARE(background->mapToScene({background->width()/2, background->height()/2}).y(), keyCenter);
        QVERIFY(background->opacity() < foreground->opacity());
        auto* ghost = f.item("otherMediaVideoClip");
        QVERIFY(ghost);
        QVERIFY(ghost->childItems().isEmpty()); // No text, handles or pointer handlers.
        const auto saved = f.host->serializeProjectState();
        const QPoint ghostCenter = ghost->mapToScene({ghost->width()/2, ghost->height()/2}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, ghostCenter);
        QTest::mouseMove(&f.view, ghostCenter + QPoint(50, 0), 20);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, ghostCenter + QPoint(50, 0));
        QVERIFY(f.timeline.selectedClipId().isEmpty());
        QCOMPARE(doc->primarySelectedMediaId(), primary->mediaId());
        QCOMPARE(f.host->serializeProjectState(), saved);

        const auto clips = timelineItems(f.view.rootObject(), "timelineVideoClip");
        QCOMPARE(clips.size(), 2);
        auto* moving = clips.first();
        auto* stationary = clips.last();
        const QPoint from = moving->mapToScene({moving->width()/2, moving->height()/2}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(&f.view, from + QPoint(qRound(2000 * f.scale()), 0), 20);
        QVERIFY(moving->z() > stationary->z());
        QVERIFY(moving->z() > ghost->z());
        QCOMPARE(f.host->serializeProjectState(), saved);
        const QPoint to = from + QPoint(qRound(4000 * f.scale()), 0);
        QTest::mouseMove(&f.view, to, 20);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(other->timelineTrack().toJson(), track.toJson());
        QCOMPARE(primary->timelineTrack().clips.size(), 2);
        QCOMPARE(primary->timelineTrack().clips.last().startSlot, 120);

        const qreal clipHeight = f.item("timelineClipTrack")->height();
        doc->clearSelection();
        QVERIFY(f.item("timelineClipTrack")->isVisible());
        QCOMPARE(f.item("timelineClipTrack")->height(), clipHeight);
        QCOMPARE(f.timeline.otherClips().size(), 3);
        QCOMPARE(timelineItems(f.view.rootObject(), "otherMediaVideoClip").size(), 3);
        auto* text = doc->addText({}, "Static media");
        doc->select(text->mediaId());
        QVERIFY(f.item("timelineClipTrack")->isVisible());
        QCOMPARE(f.item("timelineClipTrack")->height(), clipHeight);
        QCOMPARE(f.timeline.otherClips().size(), 3);
        doc->removeMedia(other->mediaId());
        QCOMPARE(timelineItems(f.view.rootObject(), "otherMediaVideoClip").size(), 2);
    }

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

    void seeksUseNearestSlotsAndContinuousTransportDoesNotSave()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host); host->setProjectEditingEnabled(true);
        auto* doc=host->document(); auto* media=doc->addText({}, "Grid"); doc->select(media->mediaId());
        TimelineController timeline; timeline.setHost(host.get());
        timeline.seek(1000.0/60); QCOMPARE(timeline.positionSlot(),1);
        QCOMPARE(timeline.positionMs(),1000.0/30);
        timeline.placeKeyframe(); timeline.seek(48); timeline.placeKeyframe();
        QCOMPARE(media->timelineTrack().keyframes.size(),1);
        timeline.stepSlots(1); QCOMPARE(timeline.positionSlot(),2);
        timeline.stepSlots(-1); QCOMPARE(timeline.positionSlot(),1);
        timeline.setStopTime(1500.0/30); QCOMPARE(doc->timelineSettings().stopSlot,2);
        timeline.removeStop();
        QSignalSpy writes(doc,&CanvasDocument::documentChanged);
        doc->setTimelinePosition(49.125);
        QCOMPARE(doc->timelinePositionMs(),49.125); QCOMPARE(timeline.positionSlot(),1);
        QCOMPARE(writes.count(),0);
        timeline.seek(1000.0/30); timeline.togglePlayback();
        QTRY_VERIFY(host->timelinePlaying());
        QTest::qWait(95); timeline.togglePlayback();
        QVERIFY(!timeline.playing());
        QCOMPARE(doc->timelinePositionMs(),doc->timelineSettings().timeMs(timeline.positionSlot()));
        QCOMPARE(writes.count(),0);
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
        track.keyframes = {{QStringLiteral("target"), 40, b->authorElementState()}};
        b->setTimelineTrack(track);
        host->document()->select(a->mediaId());
        TimelineController timeline;
        timeline.setHost(host.get());
        auto snap = timeline.snapTime(1324, 1, QString());
        QVERIFY(snap.value(QStringLiteral("snapped")).toBool());
        QCOMPARE(snap.value(QStringLiteral("timeMs")).toDouble(), 4000.0/3);
        QVERIFY(!timeline.snapTime(1323, 1, QString()).value(QStringLiteral("snapped")).toBool());
        QVERIFY(!timeline.snapTime(4000.0/3, 1, QStringLiteral("target")).value(QStringLiteral("snapped")).toBool());
        snap = timeline.snapTime(997, 1, QString(), 1000.0/3);
        QCOMPARE(snap.value(QStringLiteral("timeMs")).toDouble(), 1000.0);
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
        mime->setData("application/x-mouffette-timeline-v4", QJsonDocument(QJsonObject{
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
        track.clips = {{QStringLiteral("original"), 0, 0, 60}};
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
        QCOMPARE(media->timelineTrack().clips.first().sourceEndSlot(), 53);
        QCOMPARE(media->timelineTrack().clips.last().durationSlots, 22);
        QCOMPARE(media->timelineTrack().clips.last().endSlot(), 75);
        QCOMPARE(media->timelineTrack().keyframes.first().id, key);
        QCOMPARE(media->timelineTrack().keyframes.first().slot, 0);
        host.reset();
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }

    void firstClipResizePreservesTheOppositeEdge_data()
    {
        QTest::addColumn<bool>("trimStart");
        QTest::newRow("start") << true;
        QTest::newRow("end") << false;
    }

    void firstClipResizePreservesTheOppositeEdge()
    {
        QFETCH(bool, trimStart);
        auto& residency = MediaResidencyManager::instance();
        residency.setMemorySnapshotForTesting({8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
        const auto resetMemory = qScopeGuard([&] { residency.clearMemorySnapshotForTesting(); });
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        TimelineController timeline;
        timeline.setHost(host.get());
        QList<QVariantMap> publishedClips;
        connect(&timeline, &TimelineController::tracksChanged, &timeline, [&] {
            for (const auto& row : timeline.clips()) publishedClips.append(row.toMap());
        });
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(1100, 240);
        view.setInitialProperties({{"session", QVariantMap{
            {"timeline", QVariant::fromValue<QObject*>(&timeline)}}}});
        view.setSource(QUrl(QStringLiteral("qrc:/qt/qml/Mouffette/App/resources/qml/app/canvas/TimelinePanel.qml")));
        QCOMPARE(view.status(), QQuickView::Ready);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto* media = host->document()->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {160, 90}, true, {});
        QVERIFY(media);
        host->document()->select(media->mediaId());
        QTRY_VERIFY(media->residencyReady() && !media->timelineTrack().clips.isEmpty());
        const auto original = media->timelineTrack().clips.first();
        QVERIFY(original.durationSlots > 100);
        const auto& grid = host->document()->timelineSettings();
        const qreal sourceEnd = grid.timeMs(grid.sourceSlots(media->sourceDurationMs()));
        QVERIFY(!publishedClips.isEmpty());
        for (const auto& row : publishedClips) {
            QCOMPARE(row.value("actualSourceDurationMs").toLongLong(), media->sourceDurationMs());
            QCOMPARE(row.value("sourceDurationMs").toDouble(), sourceEnd);
        }
        view.rootObject()->setProperty("viewDurationMs", 40000.0);
        const auto findItem = [&](const QString& name) {
            QQuickItem* result = nullptr;
            auto visit = [&](auto&& self, QQuickItem* item) -> void {
                if (item->objectName() == name) result = item;
                for (auto* child : item->childItems()) self(self, child);
            };
            visit(visit, view.rootObject());
            return result;
        };
        QQuickItem* clip = findItem("timelineVideoClip");
        QVERIFY(clip);
        QQuickItem* handle = findItem(trimStart ? "timelineClipTrimStart" : "timelineClipTrimEnd");
        QVERIFY(handle);
        const QPoint press = handle->mapToScene({handle->width()/2, handle->height()/2}).toPoint();
        const auto saved = host->serializeProjectState();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, press);
        QCOMPARE(host->serializeProjectState(), saved);
        QTest::mouseMove(&view, press);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, press);
        QCOMPARE(clip->property("shownStart").toDouble(), grid.timeMs(original.startSlot));
        QCOMPARE(clip->property("shownEnd").toDouble(), grid.timeMs(original.endSlot()));
        const QPoint release = press + QPoint(trimStart ? 100 : -100, 0);
        QTest::mouseMove(&view, release, 20);
        const qint64 expectedDelta = grid.nearestSlot(100 / view.rootObject()->property("pixelsPerMs").toDouble());
        const qint64 expectedStart = original.startSlot + (trimStart ? expectedDelta : 0);
        const qint64 expectedEnd = original.endSlot() - (trimStart ? 0 : expectedDelta);
        QCOMPARE(grid.nearestSlot(clip->property("shownStart").toDouble()), expectedStart);
        QCOMPARE(grid.nearestSlot(clip->property("shownEnd").toDouble()), expectedEnd);
        QCOMPARE(host->serializeProjectState(), saved);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, release);
        QCOMPARE(media->timelineTrack().clips.size(), 1);
        const auto result = media->timelineTrack().clips.first();
        QCOMPARE(result.startSlot, expectedStart);
        QCOMPARE(result.endSlot(), expectedEnd);
        QCOMPARE(result.sourceStartSlot, original.sourceStartSlot + (trimStart ? expectedDelta : 0));

        // Re-extend the same edge to the source boundary, including its final
        // compensated slot. The first gesture must not lose that source range.
        handle = findItem(trimStart ? "timelineClipTrimStart" : "timelineClipTrimEnd");
        QVERIFY(handle);
        const QPoint extendFrom = handle->mapToScene({handle->width()/2, handle->height()/2}).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, extendFrom);
        QTest::mouseMove(&view, press, 20);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press);
        const auto extended = media->timelineTrack().clips.first();
        QCOMPARE(extended.startSlot, original.startSlot);
        QCOMPARE(extended.durationSlots, original.durationSlots);
        QCOMPARE(extended.sourceStartSlot, original.sourceStartSlot);
    }

    void productionTimelineQmlLoadsAndCapturesOnGrid()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* media = host->document()->addText({}, QStringLiteral("Timeline UI"));
        auto* other=host->document()->addText({},"Magnetic target");
        SceneTimeline::MediaTrack otherTrack;
        otherTrack.keyframes={{"target",40,other->authorElementState()}};
        other->setTimelineTrack(otherTrack);
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
        view.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&view));
        timeline.seek(1337);
        auto* capture = view.rootObject()->findChild<QQuickItem*>(QStringLiteral("timelinePlaceKeyframe"));
        QVERIFY(capture);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
            capture->mapToScene(QPointF(capture->width()/2, capture->height()/2)).toPoint());
        QTRY_COMPARE(media->timelineTrack().keyframes.size(), 1);
        QCOMPARE(media->timelineTrack().keyframes.first().slot, 40);
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
        const qreal slotMs=1000.0/30;
        QVERIFY(view.rootObject()->property("gridStep").toDouble()>slotMs);
        // Shift release immediately restores the grid preview, never free time.
        keyItem->setProperty("dragging",true); keyItem->setProperty("rawMs",1309.0);
        view.rootObject()->setProperty("activeDrag",QVariant::fromValue(keyItem));
        view.rootObject()->setProperty("shiftHeld",true);
        QCOMPARE(keyItem->property("previewMs").toDouble(),4000.0/3);
        view.rootObject()->setProperty("shiftHeld",false);
        QCOMPARE(keyItem->property("previewMs").toDouble(),1300.0);
        keyItem->setProperty("dragging",false);
        QVERIFY(QMetaObject::invokeMethod(view.rootObject(),"endDrag"));
        view.rootObject()->setProperty("viewDurationMs",500.0);
        QCOMPARE(view.rootObject()->property("gridStep").toDouble(),slotMs);
        view.rootObject()->setProperty("viewDurationMs",15000.0);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
            keyItem->mapToScene(QPointF(keyItem->width()/2, keyItem->height()/2)).toPoint());
        QTRY_VERIFY(!view.rootObject()->property("textInputFocused").toBool());
        QCOMPARE(timeline.selectedKeyframeId(), media->timelineTrack().keyframes.first().id);
        QTest::keyClick(&view, Qt::Key_Right); QTRY_COMPARE(timeline.positionSlot(),41);
        QTest::keyClick(&view, Qt::Key_Left); QTRY_COMPARE(timeline.positionSlot(),40);
        timeField->forceActiveFocus(); timeField->setProperty("text", "00:00.050");
        QTest::keyClick(&view, Qt::Key_Return); QTRY_COMPARE(timeline.positionSlot(),2);
        host->document()->removeMedia(other->mediaId());
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

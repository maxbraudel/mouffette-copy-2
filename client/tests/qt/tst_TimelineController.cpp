#include <QApplication>
#include <QClipboard>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QImage>
#include <QtTest>
#include "../fixtures/TimelineTrackRangeConfig.h"

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/scene/SceneTimeline.h"
#include "backend/media/MediaResidencyManager.h"
#include "frontend/qml/MediaSettingsViewModel.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
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
    QQuickItem* item(const QString& name) const {
        const auto items = timelineItems(view.rootObject(), name);
        if (name == "timelineClipTrack" || name == "timelineClipTrackLabel")
            for (auto* item : items) if (item->isVisible()) return item;
        return items.value(0);
    }
    void movePointer(const QPoint& point, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QMouseEvent event(QEvent::MouseMove, point, view.mapToGlobal(point), Qt::NoButton, Qt::LeftButton, modifiers);
        QCoreApplication::sendEvent(&view, &event);
    }
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
    void thumbnailsFollowResidencyViewportAndTrimPreview()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        QTemporaryDir temporary;
        const auto path = temporary.filePath("thumbnail.png");
        QImage image(120, 80, QImage::Format_RGB32); image.fill(Qt::cyan); QVERIFY(image.save(path));
        auto* doc = f.host->document();
        doc->setMediaResidencySuspended(true);
        auto* media = doc->addPreparedFile(path, image.size(), false, {}); QVERIFY(media);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        QVERIFY(!clip->property("hasThumbnails").toBool());
        QVERIFY(timelineItems(clip, "timelineThumbnails").isEmpty());
        doc->setMediaResidencySuspended(false);
        QTRY_VERIFY(media->residencyReady());
        QTRY_VERIFY(clip->property("hasThumbnails").toBool());
        auto* thumbnails = f.item("timelineThumbnails"); QVERIFY(thumbnails);
        QCOMPARE(thumbnails->property("ownerId").toString(), media->residencyOwnerId());
        auto* viewport = f.item("timelineClipViewport"); QVERIFY(viewport);
        const qreal scroll = viewport->property("contentY").toReal();
        viewport->setProperty("contentY", scroll + 10000);
        QTRY_VERIFY(timelineItems(clip, "timelineThumbnails").isEmpty());
        viewport->setProperty("contentY", scroll);
        QTRY_VERIFY(clip->property("hasThumbnails").toBool());
        doc->setMediaResidencySuspended(true);
        QTRY_VERIFY(!clip->property("hasThumbnails").toBool());
        doc->setMediaResidencySuspended(false);
        QTRY_VERIFY(clip->property("hasThumbnails").toBool());
        QVERIFY(doc->removeMedia(media->mediaId()));
        auto* text = doc->addText({}, "No thumbnails on text"); QVERIFY(text);
        clip = f.item("timelineClip"); QVERIFY(clip);
        QVERIFY(!clip->property("hasThumbnails").toBool());
        QVERIFY(timelineItems(clip, "timelineThumbnails").isEmpty());
        QVERIFY(doc->removeMedia(text->mediaId()));
        auto* video = doc->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {320, 180}, true, {}); QVERIFY(video);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady(), 15000);
        clip = f.item("timelineClip"); QVERIFY(clip);
        QTRY_VERIFY(clip->property("hasThumbnails").toBool());
        thumbnails = f.item("timelineThumbnails"); QVERIFY(thumbnails);
        // The same source offset used by playback/hold regions drives an uncommitted trim.
        const qreal initial = clip->property("shownSourceIn").toReal();
        clip->setProperty("initialStart", 0);
        clip->setProperty("previewStart", 100);
        clip->setProperty("previewEnd", 1000);
        clip->setProperty("previewTrack", clip->property("modelData").toMap().value("displayTrackIndex"));
        clip->setProperty("editEdge", -1);
        clip->setProperty("dragging", true);
        QCOMPARE(thumbnails->property("sourceInMs").toReal(), initial + 100);
        clip->setProperty("dragging", false);
        QCOMPARE(thumbnails->property("sourceInMs").toReal(), initial);
        f.view.rootObject()->setProperty("viewDurationMs", 1500);
        const auto screenshot = qEnvironmentVariable("MOUFFETTE_TIMELINE_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(f.view.grabWindow().save(screenshot));
        }
    }

    void minimumTrackRangeSurvivesEditsAndExtendsAtOccupiedEdges_data()
    {
        QTest::addColumn<int>("above");
        QTest::addColumn<int>("below");
        QTest::newRow("defaults") << 10 << 10;
        QTest::newRow("asymmetric") << 12 << 4;
    }

    void minimumTrackRangeSurvivesEditsAndExtendsAtOccupiedEdges()
    {
        QFETCH(int, above); QFETCH(int, below);
        TimelineTrackRangeConfig config(above, below); QVERIFY2(config.loaded, qPrintable(config.error));
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* viewport = f.item("timelineClipViewport"); QVERIFY(viewport);
        QCOMPARE(f.timeline.trackCount(), above + below + 1);
        QCOMPARE(f.timeline.firstTrackIndex(), -above);
        QCOMPARE(f.timeline.activeTrackIndex(), above); // Paste initially targets Track 0.
        QCOMPARE(doc->timelineTrackAtRow(above + below), below);
        const auto names = f.view.rootObject()->property("trackNames").toStringList();
        QCOMPARE(names.first(), "Track " + QString::number(above));
        QCOMPARE(names.last(), "Track " + QString::number(-below));
        QTRY_COMPARE(viewport->property("contentY").toReal(), (f.timeline.clipTrackHeightPx() - viewport->height()) / 2);
        const auto initialScroll = viewport->property("contentY").toReal();
        auto* media = doc->addText({}, "Zero"); QVERIFY(media);
        const auto id = media->timelineTrack().clip.id;
        QTest::qWait(20);
        QCOMPARE(media->timelineTrack().trackIndex, 0);
        QCOMPARE(f.timeline.trackCount(), above + below + 1);
        QCOMPARE(viewport->property("contentY").toReal(), initialScroll);
        f.timeline.moveClip(id, 0, 0);
        QCOMPARE(media->timelineTrack().trackIndex, -above);
        QCOMPARE(f.timeline.firstTrackIndex(), -above - 1);
        QCOMPARE(f.timeline.trackCount(), above + below + 2);
        f.timeline.moveClip(id, 0, f.timeline.trackCount() - 1);
        QCOMPARE(media->timelineTrack().trackIndex, below);
        QCOMPARE(f.timeline.firstTrackIndex(), -above);
        QCOMPARE(doc->timelineTrackAtRow(f.timeline.trackCount() - 1), below + 1);
        QCOMPARE(f.timeline.trackCount(), above + below + 2);
        QVERIFY(doc->removeMedia(media->mediaId()));
        QTest::qWait(20);
        QCOMPARE(f.timeline.firstTrackIndex(), -above);
        QCOMPARE(f.timeline.trackCount(), above + below + 1);
        const auto point = viewport->mapToScene({100, viewport->height()/2}).toPoint();
        f.wheel(point, {0, 10000}, {});
        QCOMPARE(viewport->property("contentY").toReal(), -above * qreal(f.timeline.clipTrackHeightPx()));
        f.wheel(point, {0, -10000}, {});
        QCOMPARE(viewport->property("contentY").toReal() + viewport->height(), (below + 1) * qreal(f.timeline.clipTrackHeightPx()));
    }

    void trackZeroStartsCenteredAndInsertionPreservesValidScrollPosition()
    {
        TimelineTrackRangeConfig config(0, 0); QVERIFY2(config.loaded, qPrintable(config.error));
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.resize(1100, 500);
        auto* viewport = f.item("timelineClipViewport"); QVERIFY(viewport);
        QTRY_COMPARE(f.view.rootObject()->height(), 500.0);
        QTRY_COMPARE(viewport->property("contentY").toReal(), (f.timeline.clipTrackHeightPx() - viewport->height()) / 2);
        QCOMPARE(f.view.rootObject()->property("trackNames").toStringList(), QStringList{"Track 0"});
        auto* doc = f.host->document();
        const qreal emptyScroll = viewport->property("contentY").toReal();
        auto* zero = doc->addText({}, "Zero"); QVERIFY(zero);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        QTest::qWait(30);
        QCOMPARE(viewport->property("contentY").toReal(), emptyScroll);
        QCOMPARE(f.view.rootObject()->property("trackNames").toStringList(), (QStringList{"Track 1", "Track 0", "Track -1"}));
        f.view.resize(1100, 240);
        QTRY_COMPARE(f.view.rootObject()->height(), 240.0);
        QTest::qWait(30);
        const qreal initialScroll = viewport->property("contentY").toReal();
        const auto position = clip->mapToScene({0, 0});
        const auto savedZero = zero->timelineTrack().toJson();
        for (int i = 1; i <= 5; ++i) {
            auto* above = doc->addText({}, "Above"); QVERIFY(above);
            QCOMPARE(above->timelineTrack().trackIndex, -i);
            QTest::qWait(20);
            QCOMPARE(clip->mapToScene({0, 0}), position);
            QCOMPARE(viewport->property("contentY").toReal(), initialScroll);
            QCOMPARE(zero->timelineTrack().toJson(), savedZero);
        }
        // Growing the lower edge and pasting a block also retain the fixed origin.
        viewport->setProperty("contentY", 0.0);
        const auto lowerPosition = clip->mapToScene({0, 0});
        doc->clearSelection();
        auto* below = doc->addText({}, "Below"); QVERIFY(below);
        QVERIFY(doc->moveTimelineClip(below->timelineTrack().clip.id, 0, 1));
        const auto copied = doc->serializeProjectState();
        QVERIFY(!doc->pasteMediaState(copied, {}).isEmpty());
        QTest::qWait(30);
        QCOMPARE(viewport->property("contentY").toReal(), 0.0);
        QCOMPARE(clip->mapToScene({0, 0}), lowerPosition);
        QCOMPARE(zero->timelineTrack().toJson(), savedZero);
        // Even after scrolling upwards, another insertion does not jump.
        viewport->setProperty("contentY", -3.5 * f.timeline.clipTrackHeightPx());
        const auto scrolled = clip->mapToScene({0, 0});
        QVERIFY(doc->addText({}, "Another upper track"));
        QTest::qWait(30);
        QCOMPARE(clip->mapToScene({0, 0}), scrolled);
    }

    void tracksUseOnlyAvailableSpaceAndNeverScrollPastTheirEdges()
    {
        TimelineTrackRangeConfig config(0, 0); QVERIFY2(config.loaded, qPrintable(config.error));
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.resize(1100, 500);
        auto* viewport = f.item("timelineClipViewport"); QVERIFY(viewport);
        auto* bar = f.item("timelineVerticalScrollBar"); QVERIFY(bar);
        QTRY_COMPARE(f.view.rootObject()->height(), 500.0);
        const qreal trackHeight = f.timeline.clipTrackHeightPx();
        auto* doc = f.host->document();
        for (int count = 0; count <= 8; ++count) {
            if (count) QVERIFY(doc->addText({}, QString::number(count)));
            QTest::qWait(20);
            const qreal top = f.timeline.firstTrackIndex() * trackHeight;
            const qreal bottom = top + f.timeline.trackCount() * trackHeight;
            const auto point = viewport->mapToScene({100, viewport->height()/2}).toPoint();
            f.wheel(point, {0, 10000}, {});
            if (bottom - top <= viewport->height()) {
                const qreal blankAbove = top - viewport->property("contentY").toReal();
                const qreal blankBelow = viewport->height() - (bottom - viewport->property("contentY").toReal());
                QCOMPARE(blankAbove, blankBelow);
                QCOMPARE(bar->property("size").toReal(), 1.0);
                const auto scroll = viewport->property("contentY").toReal();
                f.wheel(point, {0, -10000}, {});
                QCOMPARE(viewport->property("contentY").toReal(), scroll);
            } else {
                QCOMPARE(viewport->property("contentY").toReal(), top);
                f.wheel(point, {0, -10000}, {});
                QCOMPARE(viewport->property("contentY").toReal() + viewport->height(), bottom);
                QCOMPARE(bar->property("size").toReal(), viewport->height() / (bottom - top));
            }
        }
        // Shrinking the track range removes obsolete scroll space as well.
        while (!doc->media().isEmpty())
            QVERIFY(doc->removeMedia(doc->media().first()->mediaId()));
        QTRY_COMPARE(f.timeline.trackCount(), 1);
        QTRY_COMPARE(viewport->property("contentY").toReal(), (trackHeight - viewport->height()) / 2);
        QCOMPARE(bar->property("size").toReal(), 1.0);
    }

    void outerTrackSeparatorsOnlyAppearInsideTheViewport()
    {
        TimelineTrackRangeConfig config(0, 0); QVERIFY2(config.loaded, qPrintable(config.error));
        TimelineFixture f; QVERIFY(f.initialize());
        auto* viewport = f.item("timelineClipViewport"); QVERIFY(viewport);
        const qreal trackHeight = f.timeline.clipTrackHeightPx();
        for (bool occupied : {false, true}) {
            if (occupied) QVERIFY(f.host->document()->addText({}, "Track zero"));
            for (int blankSpace : {120, 0}) {
                const int viewHeight = qRound(f.view.rootObject()->height() - viewport->height()
                    + f.timeline.trackCount() * trackHeight + blankSpace);
                f.view.resize(1100, viewHeight);
                QTRY_COMPARE(f.view.rootObject()->height(), qreal(viewHeight));
                QTest::qWait(30);
                int edgesChecked = 0;
                for (const auto* name : {"timelineClipTrackHeader", "timelineClipTrack"}) {
                    for (auto* row : timelineItems(f.view.rootObject(), name)) {
                        if (!row->isVisible()) continue;
                        const int index = row->property("trackIndex").toInt();
                        if (index == f.timeline.firstTrackIndex()) {
                            auto* top = timelineItems(row, "timelineTrackTopSeparator").value(0);
                            QVERIFY(top);
                            QCOMPARE(top->isVisible(), blankSpace > 0);
                            QCOMPARE(top->mapToScene({0, 0}).y(), row->mapToScene({0, 0}).y());
                            ++edgesChecked;
                        }
                        if (index == f.timeline.firstTrackIndex() + f.timeline.trackCount() - 1) {
                            auto* bottom = timelineItems(row, "timelineTrackBottomSeparator").value(0);
                            QVERIFY(bottom);
                            QCOMPARE(bottom->isVisible(), blankSpace > 0);
                            QCOMPARE(bottom->mapToScene({0, bottom->height()}).y(), row->mapToScene({0, row->height()}).y());
                            ++edgesChecked;
                        }
                    }
                }
                QCOMPARE(edgesChecked, 4);
            }
        }
    }

    void toolbarKeepsPlaybackCenteredAndEditingSeparate()
    {
        TimelineFixture f;
        QVERIFY(f.initialize());
        auto* media = f.host->document()->addText({}, "Media name must stay off the toolbar");
        f.host->document()->select(media->mediaId());
        f.timeline.placeStop();
        auto* playback = f.item("timelinePlaybackControls");
        auto* layout = f.item("timelineLayoutControls");
        auto* edit = f.item("timelineEditBar");
        auto* play = f.item("timelinePlayPause");
        auto* editActions = f.item("timelineEditActions");
        auto* current = f.item("timelineCurrentReadout");
        auto* maximum = f.item("timelineMaximumReadout");
        auto* time = f.item("timelineCurrentTime");
        auto* frame = f.item("timelineCurrentFrame");
        QVERIFY(playback && layout && edit && editActions && play && current && maximum && time && frame);
        const auto belongsTo = [](QQuickItem* child, QQuickItem* ancestor) {
            for (auto* item = child; item; item = item->parentItem())
                if (item == ancestor) return true;
            return false;
        };
        for (const auto* name : {"timelinePlaceKeyframe", "timelineCopy", "timelinePaste",
                                 "timelineDelete", "timelinePlaceStop", "timelineRemoveStop"}) {
            auto* button = f.item(name);
            QVERIFY(button);
            QVERIFY(belongsTo(button, edit));
            QCOMPARE(button->height(), play->height());
            QVERIFY(!button->property("iconSource").toUrl().isEmpty());
        }
        QVERIFY(belongsTo(layout, editActions));
        QVERIFY(!play->property("iconOnly").toBool());
        QCOMPARE(play->property("text").toString(), QStringLiteral("Play"));
        QCOMPARE(time->height(), play->height());
        const auto timing = f.host->document()->timelineSettings();
        const auto maxFrame = QString::number(timing.maxSlot());
        QCOMPARE(f.item("timelineMaximumFrame")->property("text").toString(), "#" + maxFrame);
        QCOMPARE(frame->property("text").toString(), "#" + QString(maxFrame.size(), '0'));
        const auto timeWidth = time->width();
        const auto frameWidth = frame->width();
        const auto maximumTime = f.item("timelineMaximumTime")->property("text").toString();
        // The Stop at zero must not replace the timeline maximum or its padding.
        QVERIFY(maximumTime != time->property("text").toString());
        for (int slot : {9, 10, 99, 100}) {
            f.timeline.seek(timing.timeMs(slot));
            QTRY_COMPARE(frame->property("text").toString(), "#" + QString::number(slot).rightJustified(maxFrame.size(), '0'));
            QCOMPARE(time->width(), timeWidth);
            QCOMPARE(frame->width(), frameWidth);
            QCOMPARE(f.item("timelineMaximumTime")->property("text").toString(), maximumTime);
        }
        f.timeline.goToStart();
        for (int width : {1440, 680, 540, 320}) {
            f.view.resize(width, 240);
            QTRY_COMPARE(f.view.rootObject()->width(), qreal(width));
            QTRY_VERIFY(qAbs(playback->mapToScene({playback->width()/2, 0}).x() - width/2.0) <= 0.5);
            QTRY_VERIFY(current->mapToScene({current->width(), 0}).x() <= playback->mapToScene({0, 0}).x());
            QTRY_VERIFY(playback->mapToScene({playback->width(), 0}).x() <= maximum->mapToScene({0, 0}).x());
            QTRY_COMPARE(maximum->mapToScene({maximum->width(), 0}).x(), qreal(width - 8));
            QTRY_COMPARE(layout->mapToItem(editActions, {layout->width(), 0}).x(),
                editActions->property("contentWidth").toReal() - editActions->property("contentX").toReal() - 8);
            QCOMPARE(editActions->mapToScene({0, 0}).x(), qreal(0));
            QCOMPARE(editActions->width(), qreal(width));
            QCOMPARE(edit->height(), play->height());
            for (const auto* name : {"timelinePlaceKeyframe", "timelineCopy", "timelinePaste",
                                     "timelineDelete", "timelinePlaceStop", "timelineRemoveStop",
                                     "timelineZoomOut", "timelineZoomIn", "timelineFitDuration"}) {
                QTRY_COMPARE(f.item(name)->property("iconOnly").toBool(), width < 1440);
            }
            if (width == 540)
                QTRY_COMPARE(editActions->property("contentWidth").toReal(), editActions->width());
            QTRY_COMPARE(layout->mapToScene({0, 0}).y(), edit->mapToScene({0, 0}).y());
            auto* authoring = f.item("timelinePlaceKeyframe")->parentItem();
            const auto gap = layout->mapToScene({0, 0}).x() - authoring->mapToScene({authoring->width(), 0}).x();
            if (editActions->property("contentWidth").toReal() > editActions->width())
                QCOMPARE(gap, qreal(6));
            else
                QVERIFY(gap >= 6);
            QVERIFY(edit->mapToScene({0, 0}).y() >= playback->mapToScene({0, playback->height()}).y());
            const auto screenshotPrefix = qEnvironmentVariable("MOUFFETTE_TIMELINE_SCREENSHOT");
            if (!screenshotPrefix.isEmpty()) {
                QTest::qWait(100);
                QVERIFY(f.view.grabWindow().save(screenshotPrefix + QString::number(width) + ".png"));
            }
        }
        QVERIFY(editActions->property("contentWidth").toReal() > editActions->width());
        const auto position = f.timeline.positionMs();
        const auto zoomPosition = layout->mapToScene({0, 0});
        f.wheel(editActions->mapToScene({50, 12}).toPoint(), {0, -80}, {});
        QVERIFY(editActions->property("contentX").toReal() > 0);
        QCOMPARE(zoomPosition.x() - layout->mapToScene({0, 0}).x(), editActions->property("contentX").toReal());
        QCOMPARE(f.timeline.positionMs(), position);
        f.wheel(editActions->mapToScene({50, 12}).toPoint(), {-10000, 0}, {});
        QTRY_COMPARE(layout->mapToScene({layout->width(), 0}).x(), qreal(f.view.width() - 8));
        const auto screenshotPrefix = qEnvironmentVariable("MOUFFETTE_TIMELINE_SCREENSHOT");
        if (!screenshotPrefix.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(f.view.grabWindow().save(screenshotPrefix + QString::number(f.view.width()) + "-scrolled.png"));
        }
        // Scrolling over the now-visible zoom group moves the same content.
        const auto rightmostScroll = editActions->property("contentX").toReal();
        f.wheel(layout->mapToScene({layout->width()/2, 12}).toPoint(), {80, 0}, {});
        QCOMPARE(editActions->property("contentX").toReal(), qMax(qreal(0), rightmostScroll - 80));
        f.view.resize(1440, 240);
        QTRY_VERIFY(!play->property("iconOnly").toBool());
        QTRY_COMPARE(editActions->property("contentX").toReal(), qreal(0));
        QTRY_VERIFY(!f.item("timelinePlaceKeyframe")->property("iconOnly").toBool());
        QTRY_VERIFY(!f.item("timelineZoomIn")->property("iconOnly").toBool());
        const auto playWidth = play->width();
        f.timeline.removeStop();
        f.timeline.togglePlayback();
        QTRY_VERIFY(play->property("checked").toBool());
        QCOMPARE(play->property("text").toString(), QStringLiteral("Pause"));
        QCOMPARE(play->width(), playWidth);
        QVERIFY(play->property("iconSource").toUrl().path().endsWith("pause.svg"));
        f.timeline.togglePlayback();
        QTRY_VERIFY(!play->property("checked").toBool());
        QCOMPARE(play->property("text").toString(), QStringLiteral("Play"));
        QCOMPARE(play->width(), playWidth);
        QVERIFY(play->property("iconSource").toUrl().path().endsWith("play.svg"));
    }

    void fullPagePlayButtonAndSpaceFromCanvas_data()
    {
        QTest::addColumn<bool>("withMedia");
        QTest::newRow("empty-project") << false;
        QTest::newRow("with-media") << true;
    }

    void fullPagePlayButtonAndSpaceFromCanvas()
    {
        QFETCH(bool, withMedia);
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        if (withMedia) host->document()->addText({}, "Playback test");
        ClientWorkspaceViewModel session("playback-test", host.get(), [] {}, nullptr,
            [] { return false; }, [] { return false; }, [] { return true; });
        session.setLoading(false);
        QVERIFY(host->testSceneActionEnabled());
        QVERIFY(session.testSceneUnavailableReason().isEmpty());
        auto* timeline = qobject_cast<TimelineController*>(session.timeline());
        QVERIFY(timeline);
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(1100, 800);
        view.setInitialProperties({{"controller", QVariantMap{
            {"activeWorkspace", QVariant::fromValue<QObject*>(&session)}, {"remoteBusy", false}}}});
        view.setSource(QUrl("qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/CanvasPage.qml"));
        QVERIFY2(view.status() == QQuickView::Ready, qPrintable(view.errors().isEmpty() ? QString() : view.errors().first().toString()));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&view));
        auto* play = timelineItems(view.rootObject(), "timelinePlayPause").value(0);
        auto* loader = timelineItems(view.rootObject(), "activeCanvasLoader").value(0);
        QVERIFY(play && loader);
        auto* canvas = qvariant_cast<QQuickItem*>(loader->property("item"));
        QVERIFY(canvas);
        QSignalSpy clicks(play, SIGNAL(clicked()));
        const auto clickPlay = [&] {
            QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                play->mapToScene({play->width()/2, play->height()/2}).toPoint());
        };
        clickPlay();
        QCOMPARE(clicks.count(), 1);
        QTRY_VERIFY(timeline->playing());
        QTRY_VERIFY(timeline->positionMs() > 0);
        clickPlay();
        QCOMPARE(clicks.count(), 2);
        QTRY_VERIFY(!timeline->playing());
        const auto pausedAt = timeline->positionMs();
        QTest::qWait(80);
        QCOMPARE(timeline->positionMs(), pausedAt);
        auto* panel = timelineItems(view.rootObject(), "sceneTimeline").value(0);
        QVERIFY(panel);
        auto* toggle = timelineItems(view.rootObject(), "canvasTimelineButton").value(0);
        auto* body = timelineItems(view.rootObject(), "timelineEditorBody").value(0);
        auto* clips = timelineItems(view.rootObject(), "timelineClipViewport").value(0);
        QVERIFY(toggle && body && clips);
        for (int height : {600, 720, 640}) {
            view.resize(1100, height);
            QTRY_COMPARE(view.rootObject()->height(), qreal(height));
            QTRY_COMPARE(panel->height(), loader->height());
        }
        const qreal expandedHeight = panel->height();
        const qreal verticalScroll = clips->property("contentY").toReal();
        for (int i = 0; i < 2; ++i) {
            QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                toggle->mapToScene({toggle->width()/2, toggle->height()/2}).toPoint());
            QTRY_VERIFY(!body->isVisible());
            QCOMPARE(panel->height(), panel->property("transportHeight").toReal());
            QVERIFY(play->isVisible()); QVERIFY(loader->height() > expandedHeight);
            QVERIFY(!toggle->property("toggled").toBool());
            const auto screenshot = qEnvironmentVariable("MOUFFETTE_PAGE_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                QTest::qWait(100);
                QVERIFY(view.grabWindow().save(screenshot + "-collapsed.png"));
            }
            clickPlay(); QTRY_VERIFY(timeline->playing());
            clickPlay(); QTRY_VERIFY(!timeline->playing());
            toggle = timelineItems(view.rootObject(), "canvasTimelineButton").value(0);
            QVERIFY(toggle);
            QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                toggle->mapToScene({toggle->width()/2, toggle->height()/2}).toPoint());
            QTRY_VERIFY(body->isVisible());
            QTRY_COMPARE(panel->height(), expandedHeight);
            QTRY_COMPARE(clips->property("contentY").toReal(), verticalScroll);
        }
        const auto screenshot = qEnvironmentVariable("MOUFFETTE_PAGE_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(view.grabWindow().save(screenshot));
        }
        for (auto* focusTarget : {panel, canvas}) {
            const auto position = timeline->positionMs();
            focusTarget->forceActiveFocus();
            QTRY_VERIFY(focusTarget->hasActiveFocus());
            QTest::keyClick(&view, Qt::Key_Space);
            QTRY_VERIFY(timeline->playing());
            QTRY_VERIFY(timeline->positionMs() > position);
            focusTarget->forceActiveFocus();
            QTest::keyClick(&view, Qt::Key_Space);
            QTRY_VERIFY(!timeline->playing());
        }
    }

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
        QCOMPARE(f.timeline.positionMs(), f.timeline.gridTime(f.timeAt(220)));
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

    void rulerShiftSnapsToClipEdges_data()
    {
        QTest::addColumn<double>("viewDuration");
        QTest::addColumn<double>("edgeMs");
        for (double duration : {8000.0, 15000.0})
            for (double edge : {3000.0, 6000.0})
                QTest::newRow(qPrintable(QString("zoom-%1-edge-%2").arg(duration).arg(edge))) << duration << edge;
    }

    void rulerShiftSnapsToClipEdges()
    {
        QFETCH(double, viewDuration); QFETCH(double, edgeMs);
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        auto* doc = f.host->document();
        auto* media = doc->addText({}, "Snap target"); QVERIFY(media);
        auto track = media->timelineTrack();
        track.trackIndex = 2; track.clip.startSlot = 90; track.clip.durationSlots = 90;
        media->setTimelineTrack(track);
        f.view.rootObject()->setProperty("viewDurationMs", viewDuration);
        auto* tracks = f.item("timelineTracks");
        tracks->setProperty("contentX", 80.0);
        const int localX = qRound(12 + edgeMs * f.scale() - f.scroll() + 5);
        const auto point = tracks->mapToScene({qreal(localX), 10}).toPoint();
        const auto rawTime = f.timeline.gridTime(f.timeAt(localX));
        QVERIFY(rawTime != edgeMs);
        const auto saved = doc->serializeProjectState();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::ShiftModifier, point);
        QCOMPARE(f.timeline.positionMs(), edgeMs);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), edgeMs);
        // Pressing/releasing Shift updates a held scrub without moving the pointer.
        QTest::keyRelease(&f.view, Qt::Key_Shift);
        QCOMPARE(f.timeline.positionMs(), rawTime);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
        QTest::keyPress(&f.view, Qt::Key_Shift);
        QCOMPARE(f.timeline.positionMs(), edgeMs);
        const auto farPoint = point + QPoint(30, 0);
        f.movePointer(farPoint, Qt::ShiftModifier);
        QCOMPARE(f.timeline.positionMs(), f.timeline.gridTime(f.timeAt(localX + 30)));
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
        f.movePointer(point, Qt::ShiftModifier);
        QCOMPARE(f.timeline.positionMs(), edgeMs);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::ShiftModifier, point);
        QTest::keyRelease(&f.view, Qt::Key_Shift);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
        QVERIFY(!f.item("timelineScrubber")->property("dragging").toBool());
        QCOMPARE(doc->serializeProjectState(), saved); QCOMPARE(writes.count(), 0);
    }

    void shiftArrowsVisitDistinctClipEdgesAcrossAllTracks()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* selected = doc->addText({}, "Selected");
        auto* below = doc->addText({}, "Below the viewport");
        auto* above = doc->addText({}, "Above the viewport");
        QVERIFY(selected && below && above);
        const auto place = [](CanvasMedia* media, int row, qint64 start, qint64 end) {
            auto track = media->timelineTrack(); track.trackIndex = row;
            track.clip.startSlot = start; track.clip.durationSlots = end - start;
            media->setTimelineTrack(track);
        };
        // Creation order, selection and vertical visibility must not order the jumps.
        place(selected, 0, 121, 300); place(below, 5, 31, 121); place(above, -4, 61, 180);
        auto track = selected->timelineTrack();
        track.keyframes = {{"selected-key", 45, selected->authorElementState()}};
        selected->setTimelineTrack(track);
        doc->select(selected->mediaId()); doc->select(below->mediaId(), true);
        doc->setPrimarySelectedMedia(selected->mediaId());
        f.timeline.selectKeyframe("selected-key");
        const auto& grid = doc->timelineSettings();
        f.timeline.setStopTime(grid.timeMs(77));
        f.timeline.seek(0);
        f.view.rootObject()->setProperty("viewDurationMs", grid.timeMs(90));
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        f.view.rootObject()->forceActiveFocus();
        const auto saved = doc->serializeProjectState();
        const auto selection = doc->selectedMediaIds();
        const auto activeRow = f.timeline.activeTrackIndex();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        QSignalSpy selectionChanges(doc, &CanvasDocument::selectionChanged);
        const auto headVisible = [&] {
            auto* viewport = f.item("timelineTracks");
            const auto x = f.item("timelinePlayhead")->mapToItem(viewport, {0, 0}).x();
            return x >= 0 && x <= viewport->width();
        };
        for (qint64 slot : {31, 61, 121, 180, 300, 300}) {
            QTest::keyClick(&f.view, Qt::Key_Right, Qt::ShiftModifier);
            QCOMPARE(f.timeline.positionSlot(), slot);
            QVERIFY(headVisible());
        }
        QVERIFY(f.scroll() > 0);
        for (qint64 slot : {180, 121, 61, 31, 31}) {
            QTest::keyClick(&f.view, Qt::Key_Left, Qt::ShiftModifier);
            QCOMPARE(f.timeline.positionSlot(), slot);
            QVERIFY(headVisible());
        }
        // Ordinary arrows retain single-frame movement between boundaries.
        QTest::keyClick(&f.view, Qt::Key_Right); QCOMPARE(f.timeline.positionSlot(), 32);
        QTest::keyClick(&f.view, Qt::Key_Right, Qt::ShiftModifier); QCOMPARE(f.timeline.positionSlot(), 61);
        QTest::keyClick(&f.view, Qt::Key_Left); QCOMPARE(f.timeline.positionSlot(), 60);
        QTest::keyClick(&f.view, Qt::Key_Left, Qt::ShiftModifier); QCOMPARE(f.timeline.positionSlot(), 31);
        QCOMPARE(doc->selectedMediaIds(), selection);
        QCOMPARE(doc->primarySelectedMedia(), selected);
        QCOMPARE(f.timeline.selectedKeyframeId(), QStringLiteral("selected-key"));
        QCOMPARE(f.timeline.activeTrackIndex(), activeRow);
        QCOMPARE(f.item("timelineClipViewport")->property("contentY").toReal(), 0.0);
        QCOMPARE(doc->serializeProjectState(), saved);
        QCOMPARE(writes.count(), 0); QCOMPARE(selectionChanges.count(), 0);

        f.view.rootObject()->setProperty("expanded", false);
        QTest::keyClick(&f.view, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(f.timeline.positionSlot(), 31);
        f.view.rootObject()->setProperty("expanded", true);
        f.host->setProjectEditingEnabled(false);
        QTest::keyClick(&f.view, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(f.timeline.positionSlot(), 31);
    }

    void clipBoundaryNavigationHandlesEmptyTimelinesAndSceneEnds()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create()); QVERIFY(host);
        host->setProjectEditingEnabled(true);
        TimelineController timeline; timeline.setHost(host.get());
        auto* doc = host->document();
        const auto& grid = doc->timelineSettings();
        timeline.seek(grid.timeMs(151));
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        for (int direction : {-1, 0, 1}) {
            timeline.seekClipBoundary(direction);
            QCOMPARE(timeline.positionSlot(), 151);
        }
        QCOMPARE(writes.count(), 0);
        auto* media = doc->addText({}, "Whole scene"); QVERIFY(media);
        auto track = media->timelineTrack();
        track.clip.startSlot = 0; track.clip.durationSlots = grid.maxSlot();
        media->setTimelineTrack(track); writes.clear();
        const auto saved = doc->serializeProjectState();
        timeline.seekClipBoundary(0); QCOMPARE(timeline.positionSlot(), 151);
        timeline.seekClipBoundary(-1); QCOMPARE(timeline.positionSlot(), 0);
        timeline.seekClipBoundary(-1); QCOMPARE(timeline.positionSlot(), 0);
        timeline.seekClipBoundary(1); QCOMPARE(timeline.positionSlot(), grid.maxSlot());
        timeline.seekClipBoundary(1); QCOMPARE(timeline.positionSlot(), grid.maxSlot());
        host->setProjectEditingEnabled(false);
        timeline.seekClipBoundary(-1); QCOMPARE(timeline.positionSlot(), grid.maxSlot());
        QCOMPARE(doc->serializeProjectState(), saved); QCOMPARE(writes.count(), 0);
    }

    void navigationPreservesSelection_data()
    {
        QTest::addColumn<bool>("multiple");
        QTest::addColumn<bool>("keyframe");
        QTest::newRow("single-clip") << false << false;
        QTest::newRow("multiple-clips") << true << false;
        QTest::newRow("keyframe") << false << true;
    }

    void navigationPreservesSelection()
    {
        QFETCH(bool, multiple); QFETCH(bool, keyframe);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* media = doc->addText({}, "Selected clip");
        auto* other = doc->addText({}, "Other clip");
        auto track = media->timelineTrack();
        track.clip.startSlot = 60; track.clip.durationSlots = 60;
        track.keyframes = {{"selected-key", 60, media->authorElementState()}};
        media->setTimelineTrack(track);
        doc->select(media->mediaId());
        if (multiple) {
            doc->select(other->mediaId(), true);
            doc->setPrimarySelectedMedia(media->mediaId());
        }
        if (keyframe) f.timeline.selectKeyframe("selected-key");
        f.timeline.seek(2500);
        QVERIFY(media->clipActive());
        const auto selected = doc->selectedMediaIds();
        const auto selectedKey = f.timeline.selectedKeyframeId();
        const auto saved = doc->serializeSceneState();
        QSignalSpy selectionChanges(doc, &CanvasDocument::selectionChanged);
        const auto unchanged = [&] {
            return doc->selectedMediaIds() == selected && doc->primarySelectedMedia() == media
                && f.timeline.selectedClipId() == track.clip.id && f.timeline.selectedKeyframeId() == selectedKey
                && selectionChanges.isEmpty() && doc->serializeSceneState() == saved;
        };
        auto* tracks = f.item("timelineTracks");
        const auto from = tracks->mapToScene({400, 10}).toPoint();
        const auto to = tracks->mapToScene({550, 10}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QVERIFY(unchanged());
        QVERIFY(!media->clipActive());
        QTest::mouseMove(&f.view, to, 20);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(unchanged());
        for (const auto* name : {"timelineGoToEnd", "timelineGoToStart"}) {
            auto* button = f.item(name);
            QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene({button->width()/2, button->height()/2}).toPoint());
            QVERIFY(unchanged());
            QVERIFY(!media->clipActive());
        }
        QCOMPARE(f.timeline.positionSlot(), 0);
        f.view.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&f.view));
        f.view.rootObject()->forceActiveFocus();
        QTest::keyClick(&f.view, Qt::Key_Right);
        QCOMPARE(f.timeline.positionSlot(), 1);
        QVERIFY(unchanged());
        QTest::keyClick(&f.view, Qt::Key_Left);
        QCOMPARE(f.timeline.positionSlot(), 0);
        QVERIFY(unchanged());
        // Header clicks are informational, including the blank ruler cell.
        auto* headers = f.item("timelineTrackHeaders");
        for (qreal y : {10.0, f.timeline.rulerHeightPx() + 16.0,
                        f.timeline.rulerHeightPx() + 32.0 + f.timeline.clipTrackHeightPx()/2.0}) {
            const auto activeTrack = f.timeline.activeTrackIndex();
            QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
                headers->mapToScene({headers->width()/2, y}).toPoint());
            QVERIFY(unchanged());
            QCOMPARE(f.timeline.activeTrackIndex(), activeTrack);
            QCOMPARE(f.timeline.positionSlot(), 0);
        }
        // The empty portion of the selected clip's track still clears selection.
        auto* clipViewport = f.item("timelineClipViewport");
        QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
            clipViewport->mapToScene({550, (media->timelineTrack().trackIndex + 0.5) * f.timeline.clipTrackHeightPx()
                - clipViewport->property("contentY").toReal()}).toPoint());
        QVERIFY(doc->selectedMediaIds().isEmpty());
        QVERIFY(f.timeline.selectedKeyframeId().isEmpty());
        QCOMPARE(f.timeline.activeTrackIndex(), doc->timelineRow(media->timelineTrack().trackIndex));
        QCOMPARE(f.timeline.positionSlot(), 0);
        QCOMPARE(selectionChanges.count(), 1);
    }

    void trackBackgroundDoesNotSeek_data()
    {
        QTest::addColumn<QString>("trackName");
        QTest::newRow("keyframes") << QStringLiteral("timelineKeyframeTrack");
        QTest::newRow("clips") << QStringLiteral("timelineClipTrack");
    }

    void trackBackgroundDoesNotSeek()
    {
        QFETCH(QString, trackName);
        TimelineFixture f;
        QVERIFY(f.initialize());
        auto* track = f.item(trackName);
        auto* tracks = f.item("timelineTracks");
        QVERIFY(track && tracks);
        auto* media = f.host->document()->addText({}, "Selected");
        auto mediaTrack = media->timelineTrack();
        mediaTrack.clip.durationSlots = 30;
        media->setTimelineTrack(mediaTrack);
        f.host->document()->select(media->mediaId());
        f.timeline.seek(1000);
        const auto position = f.timeline.positionMs();
        const auto saved = f.host->serializeProjectState();
        const QPoint from = track->mapToScene({tracks->width() * 0.4, track->height() / 2}).toPoint();
        const QPoint to = from + QPoint(qRound(tracks->width() * 0.2), 0);

        QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QCOMPARE(f.timeline.positionMs(), position);
        QCOMPARE(f.host->document()->selectedMediaIds().isEmpty(), trackName == "timelineClipTrack");
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(&f.view, to, 20);
        QCOMPARE(f.timeline.positionMs(), position);
        // Entering the ruler must not turn a drag started in a track into a seek.
        const QPoint ruler = tracks->mapToScene({tracks->width() * 0.6, 10}).toPoint();
        QTest::mouseMove(&f.view, ruler, 20);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, ruler);
        QCOMPARE(f.timeline.positionMs(), position);
        QCOMPARE(f.host->serializeProjectState(), saved);
    }

    void keyframeSelectionAndDeletionKeepTheSelectedClips_data()
    {
        QTest::addColumn<bool>("useKeyboard");
        QTest::newRow("toolbar") << false;
        QTest::newRow("keyboard") << true;
    }

    void keyframeSelectionAndDeletionKeepTheSelectedClips()
    {
        QFETCH(bool, useKeyboard);
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&f.view));
        auto* doc = f.host->document();
        auto* other = doc->addText({}, "Other selected clip"); QVERIFY(other);
        auto* media = doc->addText({}, "Animated clip"); QVERIFY(media);
        doc->select(other->mediaId(), true);
        doc->select(media->mediaId());
        QCOMPARE(doc->selectedMediaIds().size(), 2);
        f.timeline.placeKeyframe();
        f.timeline.seek(1000);
        f.timeline.placeKeyframe();
        f.timeline.copySelected();
        const auto clipboard = QGuiApplication::clipboard()->mimeData()->data("application/x-mouffette-timeline-v6");
        const auto selectedIds = doc->selectedMediaIds();
        const auto clipId = media->timelineTrack().clip.id;
        const int activeRow = f.timeline.activeTrackIndex();
        QSignalSpy selectionChanges(doc, &CanvasDocument::selectionChanged);
        const auto saved = doc->serializeProjectState();
        auto* keys = f.item("timelineKeyframeTrack"); QVERIFY(keys);
        auto* tracks = f.item("timelineTracks"); QVERIFY(tracks);
        QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
            keys->mapToScene({tracks->width() * 0.6, keys->height()/2}).toPoint());
        QVERIFY(f.timeline.selectedKeyframeId().isEmpty());
        QCOMPARE(f.timeline.keyframes().size(), 2);
        QCOMPARE(doc->selectedMediaIds(), selectedIds);
        QCOMPARE(f.timeline.selectedClipId(), clipId);
        QVERIFY(!f.item("timelineDelete")->isEnabled());
        QVERIFY(!f.item("timelineCopy")->isEnabled());
        f.timeline.copySelected();
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->data("application/x-mouffette-timeline-v6"), clipboard);
        QTest::keyClick(&f.view, Qt::Key_Delete);
        QCOMPARE(doc->serializeProjectState(), saved);
        for (int remaining : {1, 0}) {
            auto* key = f.item("timelineKeyframe"); QVERIFY(key);
            QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
                key->mapToScene({key->width()/2, key->height()/2}).toPoint());
            QVERIFY(!f.timeline.selectedKeyframeId().isEmpty());
            QVERIFY(f.item("timelineDelete")->isEnabled());
            if (useKeyboard) QTest::keyClick(&f.view, Qt::Key_Delete);
            else {
                auto* button = f.item("timelineDelete");
                QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
                    button->mapToScene({button->width()/2, button->height()/2}).toPoint());
            }
            QCOMPARE(f.timeline.keyframes().size(), remaining);
            QCOMPARE(doc->selectedMediaIds(), selectedIds);
            QCOMPARE(f.timeline.selectedClipId(), clipId);
            QCOMPARE(f.timeline.activeTrackIndex(), activeRow);
            QVERIFY(f.timeline.selectedKeyframeId().isEmpty());
            QVERIFY(!f.item("timelineDelete")->isEnabled());
            const auto after = doc->serializeProjectState();
            QTest::keyClick(&f.view, Qt::Key_Backspace);
            f.timeline.deleteSelected();
            QCOMPARE(doc->serializeProjectState(), after);
        }
        QCOMPARE(selectionChanges.count(), 0);
        QVERIFY(f.item("timelinePlaceKeyframe")->isEnabled());
        QCOMPARE(doc->media().size(), 2);
    }

    void zoomAnchorsButtonsWheelAndShortcutsToHead()
    {
        TimelineFixture f;
        QVERIFY(f.initialize());
        auto* root = f.view.rootObject();
        auto* tracks = f.item("timelineTracks");
        tracks->setProperty("contentX", 350.0);
        f.timeline.seek(10000);
        const qreal headX = 12 + f.timeline.positionMs() * f.scale() - f.scroll();
        const auto click = [&](const QString& name) {
            // A scaled screen may constrain the window enough to overflow the shared row.
            auto* editActions = f.item("timelineEditActions");
            f.wheel(editActions->mapToScene({50, 12}).toPoint(), {-10000, 0}, {});
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
                    const qreal anchorX = 12 + f.timeline.positionMs() * f.scale() - f.scroll();
                    const qreal scale = f.scale();
                    const int direction = natural ? -1 : 1;
                    f.wheel(cursor, pixels ? QPoint(0, direction * 24) : QPoint(),
                        {0, direction * 120}, modifier, natural);
                    QVERIFY(f.scale() > scale);
                    QVERIFY(qAbs(f.timeAt(anchorX) - f.timeline.positionMs()) < 1e-6);
                    f.wheel(cursor, pixels ? QPoint(0, -direction * 24) : QPoint(),
                        {0, -direction * 120}, modifier, natural);
                    QVERIFY(qAbs(f.scale() - scale) < 1e-6);
                    QVERIFY(qAbs(f.timeAt(anchorX) - f.timeline.positionMs()) < 1e-6);
                }
            }
        }
        f.view.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&f.view));
        root->forceActiveFocus();
        QTest::mouseMove(&f.view, cursor);
        const qreal anchorX = 12 + f.timeline.positionMs() * f.scale() - f.scroll();
        const qreal scale = f.scale();
        QTest::keyClick(&f.view, Qt::Key_Equal, Qt::ControlModifier);
        QTRY_VERIFY(f.scale() > scale);
        QVERIFY(qAbs(f.timeAt(anchorX) - f.timeline.positionMs()) < 1e-6);
        QTest::keyClick(&f.view, Qt::Key_Minus, Qt::ControlModifier);
        QTRY_VERIFY(qAbs(f.scale() - scale) < 1e-6);
        QVERIFY(qAbs(f.timeAt(anchorX) - f.timeline.positionMs()) < 1e-6);
    }

    void clipGesturesAvoidUnlessPhysicalControlHeld()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.resize(1100, 420); QTest::qWait(20);
        f.view.raise(); f.view.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&f.view));
        // Keep one native window throughout the modifier matrix; rapidly replacing
        // active Cocoa windows can race their asynchronous activation callbacks.
        for (int edge : {-1, 0, 1}) for (bool overwrite : {false, true}) for (bool shift : {false, true}) {
            auto* doc = f.host->document();
            auto* left = doc->addText({}, "Left");
            auto* moving = doc->addText({}, "Moving");
            auto* right = doc->addText({}, "Right");
            const auto set = [](CanvasMedia* media, int start, int end) {
                auto track = media->timelineTrack(); track.trackIndex = 0;
                track.clip.startSlot = start; track.clip.durationSlots = end - start;
                media->setTimelineTrack(track);
            };
            set(left, 0, 30); set(moving, 60, 90); set(right, 120, 180);
            f.view.rootObject()->setProperty("viewDurationMs", 8000.0);
            doc->select(moving->mediaId()); QTest::qWait(30);
            QQuickItem* clip = nullptr;
            for (auto* item : timelineItems(f.view.rootObject(), "timelineClip"))
                if (item->property("modelData").toMap().value("mediaId").toString() == moving->mediaId()) clip = item;
            QVERIFY(clip);
            auto* handle = edge == 0 ? clip : timelineItems(clip,
                edge < 0 ? "timelineClipTrimStart" : "timelineClipTrimEnd").first();
            const auto from = handle->mapToScene({handle->width()/2, handle->height()/2}).toPoint();
            const int delta = qRound((edge < 0 ? -1500 : edge > 0 ? 2000 : 3000) * f.scale());
            const auto to = from + QPoint(delta, 0);
            const auto control = Qt::KeyboardModifiers(f.view.rootObject()->property("controlModifier").toInt());
            const auto key = Qt::Key(f.view.rootObject()->property("controlKey").toInt());
            const auto baseModifiers = Qt::KeyboardModifiers(shift ? Qt::ShiftModifier : Qt::NoModifier);
            const auto before = doc->serializeProjectState();
            QTest::mousePress(&f.view, Qt::LeftButton, baseModifiers, from);
            f.movePointer(to, baseModifiers);
            QVERIFY(clip->property("dragging").toBool());
            const qreal blockedStart = edge < 0 ? 1000 : edge == 0 ? 3000 : 2000;
            const qreal blockedEnd = edge < 0 ? 3000 : 4000;
            QCOMPARE(clip->property("shownStart").toReal(), blockedStart);
            QCOMPARE(clip->property("shownEnd").toReal(), blockedEnd);
            QCOMPARE(doc->serializeProjectState(), before);
            // Modifier transitions alone must refresh the preview, with no pointer movement.
            QTest::keyPress(&f.view, key, baseModifiers);
            QVERIFY(edge < 0 ? clip->property("shownStart").toReal() < blockedStart
                : clip->property(edge == 0 ? "shownStart" : "shownEnd").toReal() > (edge == 0 ? blockedStart : blockedEnd));
            QCOMPARE(doc->serializeProjectState(), before);
            QTest::keyRelease(&f.view, key, baseModifiers);
            QCOMPARE(clip->property("shownStart").toReal(), blockedStart);
            QCOMPARE(clip->property("shownEnd").toReal(), blockedEnd);
            if (overwrite) QTest::keyPress(&f.view, key, baseModifiers);
            const auto start = doc->timelineSettings().nearestSlot(clip->property("shownStart").toReal());
            const auto end = doc->timelineSettings().nearestSlot(clip->property("shownEnd").toReal());
            QTest::mouseRelease(&f.view, Qt::LeftButton, baseModifiers | (overwrite ? control : Qt::NoModifier), to);
            if (overwrite) QTest::keyRelease(&f.view, key, baseModifiers);
            QCOMPARE(moving->timelineTrack().clip.startSlot, start);
            QCOMPARE(moving->timelineTrack().clip.endSlot(), end);
            if (!overwrite) {
                QCOMPARE(left->timelineTrack().clip.endSlot(), 30);
                QCOMPARE(right->timelineTrack().clip.startSlot, 120);
            } else if (edge < 0) QCOMPARE(left->timelineTrack().clip.endSlot(), start);
            else if (edge == 0) QCOMPARE(right->timelineTrack().clip.endSlot(), start);
            else QCOMPARE(right->timelineTrack().clip.startSlot, end);
            doc->clear();
        }
    }

    void clipCanBeDroppedIntoEitherInsertionRow()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* a = doc->addText({}, "A"); auto* b = doc->addText({}, "B");
        const auto id = a->timelineTrack().clip.id;
        const auto before = doc->serializeProjectState();
        const int initialRow = doc->timelineRow(a->timelineTrack().trackIndex);
        const auto preview = f.timeline.previewClipEdit(id, 0, 1000, 0, 0, 0, 1000, initialRow, false);
        QCOMPARE(preview.value("row").toInt(), 0); QCOMPARE(doc->serializeProjectState(), before);
        f.timeline.moveClip(id, 0, 0);
        QCOMPARE(f.timeline.activeTrackIndex(), 1);
        QCOMPARE(doc->timelineRow(a->timelineTrack().trackIndex), 1);
        QVERIFY(a->z() > b->z()); QCOMPARE(f.timeline.trackCount(), 22);
        f.timeline.moveClip(id, 0, f.timeline.trackCount() - 1);
        QVERIFY(a->z() < b->z()); QCOMPARE(f.timeline.trackCount(), 22);
        QCOMPARE(f.timeline.activeTrackIndex(), 20);
        f.timeline.copySelected();
        f.timeline.setActiveTrackIndex(0);
        f.timeline.paste();
        QCOMPARE(f.timeline.activeTrackIndex(), 1);
        QVERIFY(doc->primarySelectedMedia()->z() > b->z());
    }

    void allClipsSelectableAndDragBetweenTracks()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.resize(1100, 400); QTest::qWait(20);
        auto* doc = f.host->document();
        auto* first = doc->addText({}, "First");
        auto* second = doc->addText({}, "Second");
        auto firstTrack = first->timelineTrack();
        firstTrack.clip.durationSlots = 30;
        firstTrack.keyframes = {{"first-key", 30, first->authorElementState()}};
        first->setTimelineTrack(firstTrack);
        auto secondTrack = second->timelineTrack();
        secondTrack.clip.startSlot = 120; secondTrack.clip.durationSlots = 30;
        secondTrack.keyframes = {{"second-key", 60, second->authorElementState()}};
        second->setTimelineTrack(secondTrack);
        doc->select(first->mediaId());
        QCOMPARE(f.timeline.clips().size(), 2);
        QCOMPARE(f.timeline.trackCount(), 21);
        for (const auto& row : f.timeline.clips()) {
            const auto clip = row.toMap();
            QCOMPARE(clip.value("selected").toBool(), clip.value("mediaId").toString() == first->mediaId());
        }
        const auto screenshot = qEnvironmentVariable("MOUFFETTE_TIMELINE_SCREENSHOT");
        if (!screenshot.isEmpty()) { QTest::qWait(100); QVERIFY(f.view.grabWindow().save(screenshot)); }
        QVERIFY(!second->clipActive());
        const auto position = f.timeline.positionMs();
        const auto clips = timelineItems(f.view.rootObject(), "timelineClip");
        QCOMPARE(clips.size(), 2);
        QQuickItem* target = nullptr;
        for (auto* item : clips)
            if (item->property("modelData").toMap().value("mediaId").toString() == second->mediaId()) target = item;
        QVERIFY(target);
        // Selection must retain this exact delegate throughout the pointer grab.
        const QPoint from = target->mapToScene({target->width()/2, target->height()/2}).toPoint();
        const QPoint to = from + QPoint(0, qRound(f.view.rootObject()->property("clipHeight").toReal()));
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QCOMPARE(doc->primarySelectedMediaId(), second->mediaId());
        QCOMPARE(f.timeline.selectedClipId(), secondTrack.clip.id);
        QVERIFY(target->property("modelData").toMap().value("selected").toBool());
        QCOMPARE(f.timeline.positionMs(), position);
        QTest::mouseMove(&f.view, to, 20);
        QCOMPARE(target->property("previewTrack").toInt(), doc->timelineRow(0));
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(second->timelineTrack().trackIndex, 0);
        QCOMPARE(second->timelineTrack().keyframes.first().slot, 60);
        QCOMPARE(f.timeline.trackCount(), 21);
        QCOMPARE(f.timeline.keyframes().first().toMap().value("id").toString(), QString("second-key"));
        doc->clearSelection();
        QCOMPARE(f.timeline.clips().size(), 2);
        for (const auto& row : f.timeline.clips()) QVERIFY(!row.toMap().value("selected").toBool());
        QCOMPARE(timelineItems(f.view.rootObject(), "timelineClip").size(), 2);
        QVERIFY(f.item("otherMediaClip") == nullptr);
    }

    void clipSelectionAlwaysFollowsTheDocument()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* first = doc->addText({}, "First");
        auto* second = doc->addText({}, "Second");
        doc->select(first->mediaId());
        auto* model = f.timeline.clipModel();
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        QSignalSpy removals(model, &QAbstractItemModel::rowsRemoved);
        const auto delegates = timelineItems(f.view.rootObject(), "timelineClip");
        QCOMPARE(delegates.size(), 2);
        const auto selectionMatches = [&] {
            const auto* primary = doc->primarySelectedMedia();
            if (f.timeline.selectedClipId() != (primary ? primary->timelineTrack().clip.id : QString())) return false;
            for (auto* item : timelineItems(f.view.rootObject(), "timelineClip")) {
                const auto row = item->property("modelData").toMap();
                const auto* media = doc->mediaById(row.value("mediaId").toString());
                if (row.value("selected").toBool() != media->selected()
                    || item->property("selected").toBool() != media->selected()
                    || item->z() != (media->selected() ? 2 : 1)) return false;
            }
            return f.item("timelineCopy")->isEnabled() == bool(primary)
                && f.item("timelineDelete")->isEnabled() == bool(primary);
        };
        QVERIFY(selectionMatches());
        f.timeline.clearSelection();
        QVERIFY(doc->selectedMediaIds().isEmpty());
        QVERIFY(selectionMatches());
        // Reselecting the same instance must restore commands and highlighting together.
        doc->select(first->mediaId());
        QVERIFY(selectionMatches());
        doc->select(second->mediaId(), true);
        QVERIFY(selectionMatches());
        f.timeline.selectClip(first->timelineTrack().clip.id);
        QCOMPARE(doc->selectedMediaIds().size(), 2);
        QCOMPARE(doc->primarySelectedMedia(), first);
        QVERIFY(selectionMatches());
        f.timeline.placeKeyframe();
        QVERIFY(!f.timeline.selectedKeyframeId().isEmpty());
        const auto selectedKey = f.timeline.selectedKeyframeId();
        QVERIFY(selectionMatches());
        f.timeline.copySelected();
        auto clipboard = [] {
            return QJsonDocument::fromJson(QGuiApplication::clipboard()->mimeData()->data(
                "application/x-mouffette-timeline-v6")).object();
        };
        QCOMPARE(clipboard().value("kind").toString(), QString("keyframe"));
        f.timeline.selectClip(first->timelineTrack().clip.id);
        QCOMPARE(f.timeline.selectedKeyframeId(), selectedKey);
        QVERIFY(selectionMatches());
        f.timeline.copySelected();
        QCOMPARE(clipboard().value("kind").toString(), QString("clip"));
        QCOMPARE(clipboard().value("mediaId").toString(), first->mediaId());
        f.timeline.setActiveTrackIndex(2);
        QVERIFY(doc->selectedMediaIds().isEmpty());
        QCOMPARE(f.timeline.activeTrackIndex(), 2);
        QVERIFY(selectionMatches());
        doc->select(first->mediaId());
        auto* ruler = f.item("timelineTracks");
        QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier, ruler->mapToScene({200, 10}).toPoint());
        QCOMPARE(doc->primarySelectedMedia(), first);
        QVERIFY(selectionMatches());
        doc->select(first->mediaId());
        doc->clearSelection();
        QVERIFY(selectionMatches());
        QCOMPARE(resets.count(), 0);
        QCOMPARE(removals.count(), 0);
        QCOMPARE(timelineItems(f.view.rootObject(), "timelineClip"), delegates);
        // Replacing a clip on the same instance cannot leave a stale selected ID.
        doc->select(first->mediaId());
        auto track = first->timelineTrack();
        track.clip.id = SceneTimeline::newId();
        first->setTimelineTrack(track);
        QVERIFY(selectionMatches());
        QCOMPARE(f.timeline.selectedClipId(), track.clip.id);
        f.timeline.selectClip(second->timelineTrack().clip.id);
        const auto removedId = second->mediaId();
        f.timeline.deleteSelected();
        QVERIFY(!doc->mediaById(removedId));
        QVERIFY(doc->selectedMediaIds().isEmpty());
        QVERIFY(f.timeline.selectedClipId().isEmpty());
        QVERIFY(!f.item("timelineDelete")->isEnabled());
    }

    void clipLabelsStayVisibleAndFillTrackHeight_data()
    {
        QTest::addColumn<int>("startMs");
        QTest::addColumn<int>("endMs");
        QTest::addColumn<int>("viewStartMs");
        QTest::addColumn<QString>("placement");
        QTest::newRow("long-clip-middle") << 0 << 90000 << 30000 << "center";
        QTest::newRow("left-of-center") << 0 << 5000 << 0 << "center";
        QTest::newRow("right-of-center") << 9000 << 14000 << 0 << "center";
        QTest::newRow("left-edge-offscreen") << 0 << 34000 << 30000 << "center";
        QTest::newRow("right-edge-offscreen") << 40000 << 60000 << 30000 << "center";
        QTest::newRow("short-clip") << 5000 << 5500 << 0 << "fill";
        QTest::newRow("long-title") << 0 << 90000 << 30000 << "fill";
        QTest::newRow("offscreen") << 0 << 1000 << 30000 << "hidden";
    }

    void clipLabelsStayVisibleAndFillTrackHeight()
    {
        QFETCH(int, startMs); QFETCH(int, endMs); QFETCH(int, viewStartMs); QFETCH(QString, placement);
        TimelineFixture f; QVERIFY(f.initialize());
        const bool longTitle = QByteArray(QTest::currentDataTag()) == "long-title";
        const QString name = longTitle ? QString(500, 'W') : QString("Clip title");
        auto* media = f.host->document()->addText({}, name);
        const auto grid = f.host->document()->timelineSettings();
        auto track = media->timelineTrack();
        track.clip.startSlot = grid.nearestSlot(startMs);
        track.clip.durationSlots = grid.nearestSlot(endMs) - track.clip.startSlot;
        media->setTimelineTrack(track);
        if (longTitle) {
            f.view.resize(320, 240);
            QTRY_COMPARE(f.view.rootObject()->width(), 320.0);
        }
        f.view.rootObject()->setProperty("viewDurationMs", 15000.0);
        f.item("timelineTracks")->setProperty("contentX", viewStartMs * f.scale());
        auto* clip = f.item("timelineClip");
        auto* label = f.item("timelineClipLabel");
        auto* title = f.item("timelineClipTitle");
        QVERIFY(clip && label && title);
        QVERIFY(!f.item("timelineClipDuration"));
        QVERIFY(f.item("timelineClipTrimStart") && f.item("timelineClipTrimEnd"));
        QVERIFY(f.item("timelineClipTrimStart")->childItems().isEmpty());
        QVERIFY(f.item("timelineClipTrimEnd")->childItems().isEmpty());
        const qreal trackHeight = f.view.rootObject()->property("clipHeight").toReal();
        QCOMPARE(clip->y(), 0.0);
        QCOMPARE(clip->height(), trackHeight);
        QCOMPARE(label->height(), clip->height());
        QVERIFY(f.item("timelineClipTrackLabel")->isVisible());
        QCOMPARE(title->property("text").toString(), media->displayName());
        if (placement == "hidden") {
            QCOMPARE(label->width(), 0.0);
            QVERIFY(!label->isVisible());
            return;
        }
        QTRY_VERIFY(label->width() > 0);
        QVERIFY(label->x() >= 8);
        QVERIFY(label->x() + label->width() <= clip->width() - 8 + 0.01);
        auto* viewport = f.item("timelineTracks");
        const auto centeredInVisibleClip = [&] {
            const qreal left = qMax(clip->mapToScene({0, 0}).x(), viewport->mapToScene({0, 0}).x());
            const qreal right = qMin(clip->mapToScene({clip->width(), 0}).x(),
                viewport->mapToScene({viewport->width(), 0}).x());
            if (right - left <= 16) return label->width() == 0 && !label->isVisible();
            return qAbs(label->mapToScene({label->width()/2, 0}).x() - (left + right)/2) < 0.01;
        };
        QVERIFY(label->mapToScene({0, 0}).x() >= viewport->x() + 8);
        QVERIFY(label->mapToScene({label->width(), 0}).x() <= f.view.width() - 8 + 0.01);
        QTRY_VERIFY(centeredInVisibleClip());
        if (QByteArray(QTest::currentDataTag()) == "long-clip-middle") {
            // Follow both horizontal scrolling and resizing/zooming without touching the clip.
            f.item("timelineTracks")->setProperty("contentX", 35000 * f.scale());
            f.view.resize(680, 240);
            QTRY_COMPARE(f.view.rootObject()->width(), 680.0);
            f.view.rootObject()->setProperty("viewDurationMs", 10000.0);
            QTRY_VERIFY(centeredInVisibleClip());
        } else if (longTitle) {
            QTRY_COMPARE(label->width(), viewport->width() - 16.0);
            QVERIFY(title->width() < title->implicitWidth());
        } else if (placement == "fill") {
            QCOMPARE(label->width(), clip->width() - 16);
        }
        const auto screenshot = qEnvironmentVariable("MOUFFETTE_TIMELINE_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(f.view.grabWindow().save(screenshot + QTest::currentDataTag() + ".png"));
        }
        // During trimming the title stays centered in the preview.
        clip->setProperty("previewStart", qreal(startMs));
        clip->setProperty("previewEnd", qreal(startMs + 2000));
        clip->setProperty("dragging", true);
        QCOMPARE(title->property("text").toString(), media->displayName());
        QVERIFY(centeredInVisibleClip());
        clip->setProperty("dragging", false);
        QVERIFY(centeredInVisibleClip());
    }

    void staticClipsControlPresenceWithoutChangingKeyframes()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        f.timeline.seek(2000);
        auto* media = doc->addText({}, "Presence");
        QVERIFY(media);
        const auto initial = media->timelineTrack().clip;
        QCOMPARE(initial.startSlot, 60);
        QCOMPARE(initial.durationSlots, AppConfig::instance().timelineDefaultClipDurationSlots());
        QVERIFY(!initial.sourceStartSlot); QVERIFY(media->clipActive());
        f.timeline.trimClip(initial.id, 1000, 3000);
        f.timeline.seek(0);
        QVERIFY(!media->clipActive()); QVERIFY(media->contentVisible());
        QVERIFY(f.host->controller()->selectionChromeModel().isEmpty());
        const auto saved = doc->serializeSceneState();
        f.timeline.seek(2000); QVERIFY(media->clipActive());
        f.timeline.seek(0); QCOMPARE(doc->serializeSceneState(), saved);
        f.timeline.placeKeyframe();
        const auto keyId = media->timelineTrack().keyframes.first().id;
        f.timeline.moveClip(initial.id, 4000);
        QCOMPARE(media->timelineTrack().keyframes.first().slot, 0);
        f.timeline.seek(5000); QVERIFY(f.timeline.canSplit()); f.timeline.splitClip();
        QCOMPARE(doc->media().size(), 2);
        QCOMPARE(media->timelineTrack().clip.endSlot(), 150);
        QCOMPARE(media->timelineTrack().keyframes.first().id, keyId);
        auto* right = doc->media().last();
        QVERIFY(right != media);
        QCOMPARE(right->timelineTrack().clip.startSlot, 150);
        QCOMPARE(right->timelineTrack().keyframes.first().slot, 0);
        QVERIFY(right->timelineTrack().keyframes.first().id != keyId);
        f.timeline.selectClip(right->timelineTrack().clip.id); f.timeline.copySelected();
        const auto originalRight = right->mediaId();
        f.timeline.deleteSelected();
        QVERIFY(!doc->mediaById(originalRight));
        f.timeline.setActiveTrackIndex(doc->timelineRow(0)); f.timeline.seek(8000);
        QVERIFY(f.timeline.canPaste()); f.timeline.paste();
        QCOMPARE(doc->media().size(), 2);
        auto* copy = doc->primarySelectedMedia(); QVERIFY(copy);
        QCOMPARE(copy->timelineTrack().clip.startSlot, 240);
        QCOMPARE(copy->timelineTrack().trackIndex, 0);
        QCOMPARE(copy->timelineTrack().keyframes.first().slot, 0);
        QVERIFY(!copy->timelineTrack().clip.sourceStartSlot);
        CanvasDocument restored;
        QVERIFY(restored.restoreProjectState(doc->serializeProjectState(), {}));
        QCOMPARE(restored.media().size(), 2);
        QCOMPARE(restored.timelineTrackCount(), 21);
    }

    void verticalScrollingKeepsKeysFixedAndEmptyTracksReachable()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        for (int i = 0; i < 8; ++i) QVERIFY(f.host->document()->addText({}, QString::number(i)));
        QCOMPARE(f.timeline.trackCount(), 21);
        auto* viewport = f.item("timelineClipViewport"); QVERIFY(viewport);
        const qreal keyY = f.item("timelineKeyframeTrack")->mapToScene({0, 0}).y();
        const auto head = f.timeline.positionMs();
        const auto point = viewport->mapToScene({100, 50}).toPoint();
        f.wheel(point, {0, -120}, {});
        QVERIFY(viewport->property("contentY").toReal() > (f.timeline.clipTrackHeightPx() - viewport->height()) / 2);
        QCOMPARE(f.item("timelineKeyframeTrack")->mapToScene({0, 0}).y(), keyY);
        QCOMPARE(f.timeline.positionMs(), head);
        QCOMPARE(f.scroll(), 0.0);
        f.wheel(point, {-80, 0}, {});
        QCOMPARE(f.scroll(), 80.0);
        const auto ids = f.host->document()->selectedMediaIds();
        QVERIFY(!ids.isEmpty());
    }

    void verticalScrollbarAndWheelShareTheSignedCoordinates()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        for (int i = 0; i < 8; ++i) QVERIFY(f.host->document()->addText({}, QString::number(i)));
        auto* viewport = f.item("timelineClipViewport");
        auto* bar = f.item("timelineVerticalScrollBar");
        QVERIFY(viewport && bar);
        viewport->setProperty("contentY", -4.0 * f.timeline.clipTrackHeightPx());
        const qreal initial = viewport->property("contentY").toReal();
        const auto thumb = bar->mapToScene({bar->width()/2,
            (bar->property("position").toReal() + bar->property("size").toReal()/2) * bar->height()}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, thumb);
        QVERIFY(bar->property("pressed").toBool());
        QTest::mouseMove(&f.view, thumb + QPoint(0, 12), 20);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, thumb + QPoint(0, 12));
        QVERIFY(viewport->property("contentY").toReal() > initial);
        const auto beforeWheel = viewport->property("contentY").toReal();
        const auto beforePosition = bar->property("position").toReal();
        f.wheel(viewport->mapToScene({100, 30}).toPoint(), {0, 10}, {});
        QCOMPARE(viewport->property("contentY").toReal(), beforeWheel - 10);
        QVERIFY(bar->property("position").toReal() < beforePosition);
        // Revealing an upper track below the viewport must stay above zero.
        auto* doc = f.host->document();
        auto* moved = doc->media().first();
        viewport->setProperty("contentY", -7.0 * f.timeline.clipTrackHeightPx());
        f.timeline.moveClip(moved->timelineTrack().clip.id, 10000, doc->timelineRow(-4));
        QCOMPARE(moved->timelineTrack().trackIndex, -4);
        QVERIFY(viewport->property("contentY").toReal() < 0);
        const qreal trackTop = -4.0 * f.timeline.clipTrackHeightPx() - viewport->property("contentY").toReal();
        QVERIFY(trackTop >= 0 && trackTop + f.timeline.clipTrackHeightPx() <= viewport->height());
    }

    void keyframeSeparatorStaysAboveScrollableTracks()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        for (int i = 0; i < 8; ++i) QVERIFY(f.host->document()->addText({}, QString::number(i)));
        auto* separator = f.item("timelineKeyframeSeparator");
        auto* keys = f.item("timelineKeyframeTrack");
        auto* clips = f.item("timelineClipViewport");
        auto* headers = f.item("timelineClipHeaders");
        QVERIFY(separator && keys && clips && headers);
        const auto position = separator->mapToScene({0, 0});
        QCOMPARE(position.x(), 0.0);
        QCOMPARE(position.y(), keys->mapToScene({0, keys->height()}).y());
        QCOMPARE(separator->width(), f.view.width());
        QCOMPARE(clips->mapToScene({0, 0}).y(), position.y() + separator->height());
        QCOMPARE(headers->mapToScene({0, 0}).y(), position.y() + separator->height());
        for (qreal offset : {13.0, 120.0, clips->property("contentHeight").toReal() - clips->height()}) {
            clips->setProperty("contentY", offset);
            f.item("timelineTracks")->setProperty("contentX", offset);
            QTest::qWait(30);
            QCOMPARE(separator->mapToScene({0, 0}), position);
            const auto frame = f.view.grabWindow();
            QVERIFY(!frame.isNull());
            const qreal scale = frame.width() / qreal(f.view.width());
            for (qreal x : {20.0, f.view.width() / 2.0, f.view.width() - 20.0})
                QCOMPARE(frame.pixelColor(qFloor(x * scale), qFloor((position.y() + 0.5) * scale)),
                    separator->property("color").value<QColor>());
        }
    }

    void clipPressAtViewportEdgeRequiresDrag_data()
    {
        QTest::addColumn<QString>("viewportEdge");
        QTest::addColumn<int>("editEdge");
        QTest::newRow("move-top") << "top" << 0;
        QTest::newRow("move-bottom") << "bottom" << 0;
        QTest::newRow("move-left") << "left" << 0;
        QTest::newRow("move-right") << "right" << 0;
        QTest::newRow("trim-start-top") << "top" << -1;
        QTest::newRow("trim-start-bottom") << "bottom" << -1;
        QTest::newRow("trim-end-top") << "top" << 1;
        QTest::newRow("trim-end-bottom") << "bottom" << 1;
    }

    void clipPressAtViewportEdgeRequiresDrag()
    {
        QFETCH(QString, viewportEdge); QFETCH(int, editEdge);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* media = doc->addText({}, "Click near viewport edge");
        QVERIFY(doc->addText({}, "Keep upper tracks occupied"));
        auto track = media->timelineTrack();
        track.trackIndex = 3;
        track.clip.startSlot = 300;
        track.clip.durationSlots = 300;
        media->setTimelineTrack(track);
        auto* last = doc->addText({}, "Keep lower tracks scrollable");
        auto lastTrack = last->timelineTrack();
        lastTrack.trackIndex = 8;
        last->setTimelineTrack(lastTrack);
        doc->clearSelection();
        auto* clip = f.item("timelineClip");
        auto* viewport = f.item("timelineClipViewport");
        auto* tracks = f.item("timelineTracks");
        QVERIFY(clip && viewport && tracks);
        QCOMPARE(clip->property("modelData").toMap().value("id").toString(), track.clip.id);
        const qreal localX = editEdge < 0 ? 2 : editEdge > 0 ? clip->width() - 2 : clip->width()/2;
        const qreal pointerX = viewportEdge == "left" ? 10
            : viewportEdge == "right" ? tracks->width() - f.item("timelineVerticalScrollBar")->width() - 10 : tracks->width()/2;
        const qreal pointerY = viewportEdge == "top" ? 10
            : viewportEdge == "bottom" ? viewport->height() - 18 : viewport->height()/2;
        tracks->setProperty("contentX", clip->x() + localX - pointerX);
        viewport->setProperty("contentY", clip->y() + clip->height()/2 - pointerY);
        const qreal scrollX = f.scroll();
        const qreal scrollY = viewport->property("contentY").toReal();
        const auto press = clip->mapToScene({localX, clip->height()/2}).toPoint();
        const auto saved = media->timelineTrack().toJson();
        QTest::mouseMove(&f.view, press);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, press);
        QCOMPARE(f.timeline.selectedClipId(), track.clip.id);
        QTest::qWait(120); // Several auto-scroll timer ticks with a stationary pointer.
        QVERIFY(!clip->property("dragging").toBool());
        QCOMPARE(f.scroll(), scrollX);
        QCOMPARE(viewport->property("contentY").toReal(), scrollY);
        const auto jitter = press + QPoint(qMax(1, QGuiApplication::styleHints()->startDragDistance()/2), 0);
        QTest::mouseMove(&f.view, jitter, 20);
        QTest::keyPress(&f.view, Qt::Key_Shift);
        QTest::qWait(120);
        QVERIFY(!clip->property("dragging").toBool());
        QCOMPARE(f.scroll(), scrollX);
        QCOMPARE(viewport->property("contentY").toReal(), scrollY);
        QTest::keyRelease(&f.view, Qt::Key_Shift);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, jitter);
        QCOMPARE(media->timelineTrack().toJson(), saved);
        QCOMPARE(doc->media().size(), 3);
    }

    void trackHeadersMeasureAllNamesAndFollowVerticalScroll()
    {
        TimelineTrackRangeConfig config(0, 0); QVERIFY2(config.loaded, qPrintable(config.error));
        TimelineFixture f; QVERIFY(f.initialize());
        auto* headers = f.item("timelineTrackHeaders");
        auto* keyHeader = f.item("timelineKeyframeHeader");
        auto* tracks = f.item("timelineTracks");
        auto* clips = f.item("timelineClipViewport");
        QVERIFY(headers && keyHeader && tracks && clips);
        auto* media = f.host->document()->addText({}, "Last occupied track");
        auto* anchor = f.host->document()->addText({}, "First occupied track");
        const auto checkLayout = [&] {
            const QFontMetricsF metrics(keyHeader->property("font").value<QFont>());
            qreal widest = metrics.horizontalAdvance("Keyframes");
            for (int i = 0; i < f.timeline.trackCount(); ++i)
                widest = qMax(widest, metrics.horizontalAdvance("Track " + QString::number(-f.timeline.firstTrackIndex() - i)));
            if (qAbs(headers->width() - (qCeil(widest) + 25)) > 1) return false;
            if (headers->x() != 0 || tracks->x() != headers->width()) return false;
            for (auto* header : timelineItems(f.view.rootObject(), "timelineClipTrackHeader")) {
                if (!header->isVisible()) continue;
                const int index = header->property("trackIndex").toInt();
                auto* label = timelineItems(header, "timelineClipTrackLabel").value(0);
                if (!label || label->width() + 0.01 < label->implicitWidth()
                    || label->property("text").toString() != "Track " + QString::number(-index)) return false;
                QQuickItem* row = nullptr;
                for (auto* candidate : timelineItems(f.view.rootObject(), "timelineClipTrack"))
                    if (candidate->property("trackIndex").toInt() == index) row = candidate;
                if (!row || header->mapToScene({0, 0}).y() != row->mapToScene({0, 0}).y()) return false;
            }
            return keyHeader->width() >= keyHeader->implicitWidth();
        };
        const auto keyPosition = keyHeader->mapToScene({0, 0});
        for (int count : {9, 10, 99, 100}) {
            auto track = media->timelineTrack();
            track.trackIndex = count - 4;
            media->setTimelineTrack(track);
            QCOMPARE(f.timeline.trackCount(), count);
            QTRY_VERIFY(checkLayout());
            const qreal width = headers->width();
            tracks->setProperty("contentX", 1500.0);
            QCOMPARE(headers->width(), width);
            const qreal lastScroll = clips->property("contentHeight").toReal() - clips->height();
            clips->setProperty("contentY", lastScroll);
            QTRY_VERIFY(checkLayout());
            QCOMPARE(keyHeader->mapToScene({0, 0}), keyPosition);
            bool lastLabelVisible = false;
            for (auto* label : timelineItems(f.view.rootObject(), "timelineClipTrackLabel"))
                lastLabelVisible |= label->isVisible() && label->property("text").toString() == "Track " + QString::number(-(f.timeline.firstTrackIndex() + count - 1));
            QVERIFY(lastLabelVisible);
            // Wheel events over the fixed labels scroll the same clip content.
            const auto point = headers->mapToScene({headers->width()/2,
                f.timeline.rulerHeightPx() + 32.0 + 20}).toPoint();
            f.wheel(point, {0, 13}, {});
            QCOMPARE(clips->property("contentY").toReal(), lastScroll - 13);
            QCOMPARE(f.scroll(), 1500.0);
            QTRY_VERIFY(checkLayout());
            QCOMPARE(keyHeader->mapToScene({0, 0}), keyPosition);
        }
        const auto screenshot = qEnvironmentVariable("MOUFFETTE_TIMELINE_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(f.view.grabWindow().save(screenshot + "track-headers.png"));
        }
        f.host->document()->removeMedia(media->mediaId());
        f.host->document()->removeMedia(anchor->mediaId());
        QCOMPARE(f.timeline.trackCount(), 1);
        QTRY_COMPARE(clips->property("contentY").toReal(), (f.timeline.clipTrackHeightPx() - clips->height()) / 2);
        QTRY_VERIFY(checkLayout());
    }

    void activeTrackHasNoBackgroundTint()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* tracks = f.item("timelineTracks");
        // Put a clip below track zero, then select its empty neighbour.
        auto* media = f.host->document()->addText({}, "Clip");
        auto track = media->timelineTrack(); track.trackIndex = 1;
        media->setTimelineTrack(track);
        f.timeline.setActiveTrackIndex(f.host->document()->timelineRow(1));
        auto* viewport = f.item("timelineClipViewport");
        viewport->setProperty("contentY", 0.0);
        QTest::qWait(30);
        const QPoint point = viewport->mapToScene({tracks->width()*0.6, f.timeline.clipTrackHeightPx()/2.0}).toPoint();
        const auto colorAt = [&](const QImage& frame) {
            return frame.pixelColor(qRound(point.x() * frame.width() / qreal(f.view.width())),
                qRound(point.y() * frame.height() / qreal(f.view.height())));
        };
        const QImage before = f.view.grabWindow(); QVERIFY(!before.isNull());
        QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(f.timeline.activeTrackIndex(), f.host->document()->timelineRow(0));
        QTest::qWait(30);
        const QImage after = f.view.grabWindow(); QVERIFY(!after.isNull());
        QCOMPARE(colorAt(after), colorAt(before));
    }

    void clipAutoScrollUsesConfiguredSpeed_data()
    {
        QTest::addColumn<int>("speed");
        QTest::addColumn<int>("direction");
        for (const int speed : {0, 48, 96, 192}) {
            QTest::newRow(qPrintable(QString("%1-top-left").arg(speed))) << speed << -1;
            QTest::newRow(qPrintable(QString("%1-bottom-right").arg(speed))) << speed << 1;
        }
    }

    void clipAutoScrollUsesConfiguredSpeed()
    {
        QFETCH(int, speed); QFETCH(int, direction);
        const AppConfig previous = AppConfig::instance();
        const auto restoreConfig = qScopeGuard([&] { AppConfig::instance() = previous; });
        AppConfig::LoadOptions options;
        options.defaultEnvFilePath.clear();
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_AUTO_SCROLL_SPEED_PX_PER_SECOND", QString::number(speed));
        QString error;
        QVERIFY2(AppConfig::instance().load(options, &error), qPrintable(error));
        TimelineFixture f; QVERIFY(f.initialize());
        QCOMPARE(f.timeline.autoScrollSpeedPxPerSecond(), speed);
        auto* media = f.host->document()->addText({}, "Scroll speed"); QVERIFY(media);
        f.timeline.trimClip(media->timelineTrack().clip.id, 0, f.timeline.maxDurationMs());
        auto* viewport = f.item("timelineClipViewport");
        auto* tracks = f.item("timelineTracks");
        auto* clip = f.item("timelineClip");
        tracks->setProperty("contentX", 400.0);
        viewport->setProperty("contentY", 0.0);
        const auto from = viewport->mapToScene({100, 24}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        const auto release = qScopeGuard([&] {
            QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        });
        // Advance edge scrolling with a controlled clock, independent of the test machine's frame rate.
        const auto edge = viewport->mapToItem(f.view.rootObject(), direction < 0
            ? QPointF(10, 10) : QPointF(viewport->width() - 10, viewport->height() - 10));
        clip->setProperty("pointerPanelX", edge.x());
        clip->setProperty("pointerPanelY", edge.y());
        for (int tick = 0; tick < 20; ++tick) {
            QVERIFY(QMetaObject::invokeMethod(f.view.rootObject(), "autoScrollClip",
                Q_ARG(QVariant, QVariant::fromValue<QObject*>(clip)), Q_ARG(QVariant, 16)));
        }
        const qreal distance = direction * speed * 0.32;
        QVERIFY(qAbs(f.scroll() - (400 + distance)) < 0.001);
        QVERIFY(qAbs(viewport->property("contentY").toReal() - distance) < 0.001);
        // A delayed frame must not teleport the viewport by several seconds of scrolling.
        QVERIFY(QMetaObject::invokeMethod(f.view.rootObject(), "autoScrollClip",
            Q_ARG(QVariant, QVariant::fromValue<QObject*>(clip)), Q_ARG(QVariant, 2000)));
        const qreal afterStall = direction * speed * 0.37;
        QVERIFY(qAbs(f.scroll() - (400 + afterStall)) < 0.001);
        QVERIFY(qAbs(viewport->property("contentY").toReal() - afterStall) < 0.001);
    }

    void clipDragScrollsAtTemporalViewportEdges()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* media = f.host->document()->addText({}, "Drag");
        f.timeline.trimClip(media->timelineTrack().clip.id, 0, f.timeline.maxDurationMs());
        auto* tracks = f.item("timelineTracks");
        auto* clip = f.item("timelineClip");
        auto* headers = f.item("timelineTrackHeaders");
        tracks->setProperty("contentX", 400.0);
        const auto saved = media->timelineTrack().toJson();
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        const auto from = tracks->mapToScene({100, f.timeline.rulerHeightPx() + 32.0 + 24}).toPoint();
        const auto leftEdge = tracks->mapToScene({10, f.timeline.rulerHeightPx() + 32.0 + 24}).toPoint();
        QVERIFY(leftEdge.x() > 22); // The panel edge is outside the temporal viewport.
        QTest::mouseMove(&f.view, from);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::OpenHandCursor);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QVERIFY(!clip->property("dragging").toBool());
        QCOMPARE(f.view.cursor().shape(), Qt::OpenHandCursor);
        QTest::mouseMove(&f.view, leftEdge, 20);
        QVERIFY(clip->property("dragging").toBool());
        QTRY_COMPARE(f.view.cursor().shape(), Qt::ClosedHandCursor);
        QTRY_VERIFY(f.scroll() < 400);
        QCOMPARE(headers->x(), 0.0);
        const auto outside = tracks->mapToScene({-10, f.timeline.rulerHeightPx() + 32.0 + 24}).toPoint();
        QTest::mouseMove(&f.view, outside, 20);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::ClosedHandCursor);
        const qreal scrolled = f.scroll();
        const auto rightEdge = tracks->mapToScene({tracks->width() - 10,
            f.timeline.rulerHeightPx() + 32.0 + 24}).toPoint();
        QTest::mouseMove(&f.view, rightEdge, 20);
        QTRY_VERIFY(f.scroll() > scrolled);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, rightEdge);
        QVERIFY(!clip->property("dragging").toBool());
        QTest::mouseMove(&f.view, from); // Move off the vertical scrollbar and back onto the clip.
        QTRY_COMPARE(f.view.cursor().shape(), Qt::OpenHandCursor);
        QCOMPARE(media->timelineTrack().toJson(), saved); // Full-scene clip cannot move in time.
    }

    void clipResizeHandlesStraddleEdges_data()
    {
        QTest::addColumn<int>("edge");
        QTest::addColumn<bool>("outside");
        QTest::newRow("start-inside") << -1 << false;
        QTest::newRow("start-outside") << -1 << true;
        QTest::newRow("end-inside") << 1 << false;
        QTest::newRow("end-outside") << 1 << true;
    }

    void clipResizeHandlesStraddleEdges()
    {
        QFETCH(int, edge); QFETCH(bool, outside);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* media = f.host->document()->addText({}, "Resize"); QVERIFY(media);
        f.timeline.trimClip(media->timelineTrack().clip.id, 1000, 4000);
        f.view.rootObject()->setProperty("viewDurationMs", 6000.0);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        QTest::mouseMove(&f.view, clip->mapToScene({clip->width()/2, clip->height()/2}).toPoint());
        QTRY_COMPARE(f.view.cursor().shape(), Qt::OpenHandCursor);
        const qreal edgeX = edge < 0 ? 0 : clip->width();
        const auto from = clip->mapToScene({edgeX + edge * (outside ? 2 : -2), clip->height()/2}).toPoint();
        const auto to = from + QPoint(-edge * 40, 0);
        const auto original = media->timelineTrack().clip;
        QTest::mouseMove(&f.view, from);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SizeHorCursor);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QCOMPARE(clip->property("editEdge").toInt(), edge);
        QTest::mouseMove(&f.view, to, 20);
        QVERIFY(clip->property("dragging").toBool());
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SizeHorCursor);
        const auto& grid = f.host->document()->timelineSettings();
        const qint64 trimmedSlots = grid.nearestSlot(40 / f.scale());
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(media->timelineTrack().clip.startSlot, original.startSlot + (edge < 0 ? trimmedSlots : 0));
        QCOMPARE(media->timelineTrack().clip.endSlot(), original.endSlot() - (edge > 0 ? trimmedSlots : 0));
    }

    void smallClipsMoveFromTheirBodyAndEdgeZones_data()
    {
        QTest::addColumn<int>("durationSlots");
        QTest::addColumn<int>("region");
        for (const int duration : {1, 9})
            for (int region = -2; region <= 2; ++region)
                QTest::newRow(qPrintable(QString("slots-%1-region-%2").arg(duration).arg(region)))
                    << duration << region;
    }

    void smallClipsMoveFromTheirBodyAndEdgeZones()
    {
        QFETCH(int, durationSlots); QFETCH(int, region);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* media = doc->addText({}, "Small"); QVERIFY(media);
        auto track = media->timelineTrack();
        track.clip.startSlot = 120; track.clip.durationSlots = durationSlots;
        media->setTimelineTrack(track);
        const auto& grid = doc->timelineSettings();
        f.view.rootObject()->setProperty("viewDurationMs",
            grid.timeMs(1) * (f.item("timelineTracks")->width() - 24) / 2);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        QVERIFY(clip->width() < 24);
        const qreal x = region == -2 ? -2 : region == -1 ? 1 : region == 0 ? clip->width()/2
            : region == 1 ? clip->width() - 1 : clip->width() + 2;
        const auto from = clip->mapToScene({x, clip->height()/2}).toPoint();
        const auto to = from + QPoint(40, 0);
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        QTest::mouseMove(&f.view, from);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::OpenHandCursor);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QCOMPARE(clip->property("editEdge").toInt(), 0);
        QCOMPARE(f.timeline.selectedClipId(), track.clip.id);
        f.movePointer(to);
        QVERIFY(clip->property("dragging").toBool());
        QTRY_COMPARE(f.view.cursor().shape(), Qt::ClosedHandCursor);
        QCOMPARE(writes.count(), 0);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(media->timelineTrack().clip.startSlot, track.clip.startSlot + grid.nearestSlot(40 / f.scale()));
        QCOMPARE(media->timelineTrack().clip.durationSlots, track.clip.durationSlots);
        QCOMPARE(writes.count(), 1);
    }

    void clipEdgeZonesStayFixedWhenZoomingAndResizingTheViewport_data()
    {
        QTest::addColumn<int>("handleWidth"); QTest::addColumn<int>("minimumWidth");
        QTest::newRow("defaults") << 8 << 24;
        QTest::newRow("custom") << 12 << 40;
        QTest::newRow("no-threshold") << 4 << 0;
    }

    void clipEdgeZonesStayFixedWhenZoomingAndResizingTheViewport()
    {
        QFETCH(int, handleWidth); QFETCH(int, minimumWidth);
        const AppConfig previous = AppConfig::instance();
        const auto restore = qScopeGuard([&] { AppConfig::instance() = previous; });
        AppConfig::LoadOptions options;
        options.defaultEnvFilePath.clear();
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX", QString::number(handleWidth));
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX", QString::number(handleWidth / 2));
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_CLIP_MIN_RESIZE_WIDTH_PX", QString::number(minimumWidth));
        QString error;
        QVERIFY2(AppConfig::instance().load(options, &error), qPrintable(error));
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* media = doc->addText({}, "Zoom"); QVERIFY(media);
        auto track = media->timelineTrack();
        track.clip.startSlot = 30; track.clip.durationSlots = 30;
        media->setTimelineTrack(track);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        for (const int viewportWidth : {1100, 680}) {
            f.view.resize(viewportWidth, 240);
            QTRY_COMPARE(f.view.rootObject()->width(), qreal(viewportWidth));
            for (const qreal clipWidth : {120.0, qreal(qMax(12, minimumWidth)),
                                         qreal(qMax(12, minimumWidth - 1)), 12.0, 48.0}) {
                f.view.rootObject()->setProperty("viewDurationMs",
                    doc->timelineSettings().timeMs(30) * (f.item("timelineTracks")->width() - 24) / clipWidth);
                QVERIFY(qAbs(clip->width() - clipWidth) < 0.001);
                for (const auto* name : {"timelineClipTrimStart", "timelineClipTrimEnd"}) {
                    auto* handle = f.item(name); QVERIFY(handle);
                    const auto extent = handle->mapToScene({handle->width(), 0}) - handle->mapToScene({0, 0});
                    QCOMPARE(extent.x(), qreal(handleWidth));
                }
                const auto from = clip->mapToScene({1, clip->height()/2}).toPoint();
                QTest::mouseMove(&f.view, from);
                QTRY_COMPARE(f.view.cursor().shape(), clipWidth >= minimumWidth ? Qt::SizeHorCursor : Qt::OpenHandCursor);
                QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
                QCOMPARE(clip->property("editEdge").toInt(), clipWidth >= minimumWidth ? -1 : 0);
                QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, from);
            }
        }
        QCOMPARE(media->timelineTrack().toJson(), track.toJson());
    }

    void clipResizeCanCrossTheSmallClipThreshold_data()
    {
        QTest::addColumn<int>("edge");
        QTest::newRow("start") << -1;
        QTest::newRow("end") << 1;
    }

    void clipResizeCanCrossTheSmallClipThreshold()
    {
        QFETCH(int, edge);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* media = doc->addText({}, "Shrink"); QVERIFY(media);
        auto track = media->timelineTrack();
        track.clip.startSlot = 120; track.clip.durationSlots = 30;
        media->setTimelineTrack(track);
        const auto& grid = doc->timelineSettings();
        f.view.rootObject()->setProperty("viewDurationMs",
            grid.timeMs(1) * (f.item("timelineTracks")->width() - 24) / 2);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        const auto from = clip->mapToScene({edge < 0 ? 1.0 : clip->width() - 1, clip->height()/2}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        const auto to = from + QPoint(-edge * 42, 0);
        f.movePointer(to);
        QVERIFY(clip->width() < 24);
        QVERIFY(clip->property("dragging").toBool());
        QCOMPARE(clip->property("editEdge").toInt(), edge);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SizeHorCursor);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(media->timelineTrack().clip.durationSlots, 9);
        QCOMPARE(media->timelineTrack().clip.startSlot, track.clip.startSlot + (edge < 0 ? 21 : 0));
        const auto nextPress = clip->mapToScene({edge < 0 ? 1.0 : clip->width() - 1, clip->height()/2}).toPoint();
        QTest::mouseMove(&f.view, nextPress);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::OpenHandCursor);
        QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier, nextPress);
        QCOMPARE(clip->property("editEdge").toInt(), 0);
    }

    void smallClipBodyWinsOverNeighbourResizeZone_data()
    {
        QTest::addColumn<bool>("leftSide"); QTest::addColumn<bool>("selectSmall");
        for (const bool left : {true, false})
            for (const bool selected : {true, false})
                QTest::newRow(qPrintable(QString("left-%1-selected-%2").arg(left).arg(selected))) << left << selected;
    }

    void smallClipBodyWinsOverNeighbourResizeZone()
    {
        QFETCH(bool, leftSide); QFETCH(bool, selectSmall);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* large = doc->addText({}, "Large");
        auto* small = doc->addText({}, "Small"); QVERIFY(large && small);
        auto largeTrack = large->timelineTrack();
        largeTrack.trackIndex = 0; largeTrack.clip.startSlot = 120; largeTrack.clip.durationSlots = 60;
        large->setTimelineTrack(largeTrack);
        auto smallTrack = small->timelineTrack();
        smallTrack.trackIndex = 0; smallTrack.clip.startSlot = leftSide ? 117 : 180; smallTrack.clip.durationSlots = 3;
        small->setTimelineTrack(smallTrack);
        f.timeline.selectClip((selectSmall ? smallTrack : largeTrack).clip.id);
        const auto& grid = doc->timelineSettings();
        f.view.rootObject()->setProperty("viewDurationMs",
            grid.timeMs(1) * (f.item("timelineTracks")->width() - 24) / 2);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        QQuickItem* clip = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClip"))
            if (item->property("modelData").toMap().value("id").toString() == smallTrack.clip.id) clip = item;
        QVERIFY(clip);
        const auto from = clip->mapToScene({leftSide ? 1 : clip->width() - 1, clip->height()/2}).toPoint();
        const auto to = from + QPoint(leftSide ? -40 : 40, 0);
        QTest::mouseMove(&f.view, from);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::OpenHandCursor);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QCOMPARE(f.timeline.selectedClipId(), smallTrack.clip.id);
        QCOMPARE(clip->property("editEdge").toInt(), 0);
        f.movePointer(to);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(small->timelineTrack().clip.startSlot, smallTrack.clip.startSlot + (leftSide ? -20 : 20));
        QCOMPARE(small->timelineTrack().clip.durationSlots, 3);
        QCOMPARE(large->timelineTrack().toJson(), largeTrack.toJson());
    }

    void neighbouringClipResizeChoosesNearestBody_data()
    {
        QTest::addColumn<int>("gapSlots");
        QTest::addColumn<bool>("jointZone");
        QTest::addColumn<bool>("leftSide");
        QTest::addColumn<bool>("selectTarget");
        QTest::addColumn<int>("direction");
        for (const int gap : {0, 1})
            for (const bool joint : {true, false}) {
                if (gap > 0 && joint) continue;
                for (const bool left : {true, false})
                    for (const bool selected : {true, false})
                        for (const int direction : {-1, 1})
                            QTest::newRow(qPrintable(QString("gap-%1-joint-%2-%3-selected-%4-direction-%5").arg(gap).arg(joint)
                                .arg(left ? "left" : "right").arg(selected).arg(direction))) << gap << joint << left << selected << direction;
            }
    }

    void neighbouringClipResizeChoosesNearestBody()
    {
        QFETCH(int, gapSlots); QFETCH(bool, leftSide); QFETCH(bool, selectTarget);
        QFETCH(int, direction); QFETCH(bool, jointZone);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* left = doc->addText({}, "Left");
        auto* right = doc->addText({}, "Right");
        QVERIFY(left && right);
        auto leftTrack = left->timelineTrack();
        leftTrack.trackIndex = 0; leftTrack.clip.startSlot = 30; leftTrack.clip.durationSlots = 60;
        left->setTimelineTrack(leftTrack);
        auto rightTrack = right->timelineTrack();
        rightTrack.trackIndex = 0; rightTrack.clip.startSlot = 90 + gapSlots; rightTrack.clip.durationSlots = 60;
        right->setTimelineTrack(rightTrack);
        const bool trimLeft = jointZone || leftSide; // The dedicated junction handle belongs to the left clip.
        auto* target = trimLeft ? left : right;
        auto* neighbour = trimLeft ? right : left;
        f.timeline.selectClip((selectTarget ? target : neighbour)->timelineTrack().clip.id);
        f.view.rootObject()->setProperty("viewDurationMs", 6000.0);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        QQuickItem* leftItem = nullptr;
        QQuickItem* rightItem = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClip")) {
            const auto id = item->property("modelData").toMap().value("id").toString();
            if (id == leftTrack.clip.id) leftItem = item;
            if (id == rightTrack.clip.id) rightItem = item;
        }
        QVERIFY(leftItem && rightItem);
        const qreal midpoint = (leftItem->x() + leftItem->width() + rightItem->x()) / 2;
        const qreal offset = gapSlots == 0 && !jointZone
            ? (f.timeline.clipResizeHandleWidthPx() + f.timeline.clipJointResizeHandleWidthPx()) / 2.0 : 1;
        const auto from = leftItem->parentItem()->mapToScene({midpoint + (leftSide ? -offset : offset),
            leftItem->y() + leftItem->height()/2}).toPoint();
        const auto to = from + QPoint(direction * 40, 0);
        const auto original = target->timelineTrack().clip;
        const auto neighbourBefore = neighbour->timelineTrack().toJson();
        const auto saved = doc->serializeProjectState();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        QTest::mouseMove(&f.view, from);
        QTRY_COMPARE(f.view.cursor().shape(), jointZone ? Qt::SplitHCursor : Qt::SizeHorCursor);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QCOMPARE(f.timeline.selectedClipId(), original.id);
        QCOMPARE((trimLeft ? leftItem : rightItem)->property("editEdge").toInt(), trimLeft ? 1 : -1);
        QCOMPARE((trimLeft ? leftItem : rightItem)->property("editRolling").toBool(), jointZone);
        QTest::mouseMove(&f.view, to, 20);
        QTRY_COMPARE(f.view.cursor().shape(), jointZone ? Qt::SplitHCursor : Qt::SizeHorCursor);
        const qint64 trimmedSlots = doc->timelineSettings().nearestSlot(40 / f.scale());
        const qint64 delta = jointZone ? direction * trimmedSlots
            : leftSide ? qMin<qint64>(gapSlots, direction * trimmedSlots)
                       : qMax<qint64>(-gapSlots, direction * trimmedSlots);
        if (jointZone) {
            QCOMPARE(leftItem->property("shownEnd").toReal(), doc->timelineSettings().timeMs(90 + delta));
            QCOMPARE(rightItem->property("shownStart").toReal(), leftItem->property("shownEnd").toReal());
            QCOMPARE(leftItem->property("shownStart").toReal(), 1000.0);
            QCOMPARE(rightItem->property("shownEnd").toReal(), 5000.0);
        } else if (gapSlots == 0 && delta != 0) {
            auto* leftHandle = timelineItems(leftItem, "timelineClipTrimEnd").first();
            auto* rightHandle = timelineItems(rightItem, "timelineClipTrimStart").first();
            QCOMPARE(leftHandle->x() + leftHandle->width()/2, leftItem->width());
            QCOMPARE(rightHandle->x() + rightHandle->width()/2, 0.0);
            for (auto* joint : timelineItems(f.view.rootObject(), "timelineClipJointTrim"))
                QVERIFY(!joint->isVisible());
        }
        QCOMPARE(doc->serializeProjectState(), saved); QCOMPARE(writes.count(), 0);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(target->timelineTrack().clip.startSlot, original.startSlot + (trimLeft ? 0 : delta));
        QCOMPARE(target->timelineTrack().clip.endSlot(), original.endSlot() + (trimLeft ? delta : 0));
        if (jointZone) {
            QCOMPARE(left->timelineTrack().clip.endSlot(), right->timelineTrack().clip.startSlot);
            QCOMPARE(left->timelineTrack().clip.startSlot, 30);
            QCOMPARE(right->timelineTrack().clip.endSlot(), 150);
        } else QCOMPARE(neighbour->timelineTrack().toJson(), neighbourBefore);
        QCOMPARE(writes.count(), delta != 0 ? 1 : 0);
    }

    void junctionCursorTracksTheHoveredZoneBeforePress_data()
    {
        QTest::addColumn<int>("normalWidth"); QTest::addColumn<int>("jointWidthPx");
        QTest::newRow("equal-widths") << 8 << 8;
        QTest::newRow("wider-joint") << 12 << 20;
    }

    void junctionCursorTracksTheHoveredZoneBeforePress()
    {
        QFETCH(int, normalWidth); QFETCH(int, jointWidthPx);
        const AppConfig previous = AppConfig::instance();
        const auto restore = qScopeGuard([&] { AppConfig::instance() = previous; });
        AppConfig::LoadOptions options; options.defaultEnvFilePath.clear();
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX", QString::number(normalWidth));
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX", QString::number(jointWidthPx));
        QString error; QVERIFY2(AppConfig::instance().load(options, &error), qPrintable(error));
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* left = doc->addText({}, "Left");
        auto* right = doc->addText({}, "Right"); QVERIFY(left && right);
        auto leftTrack = left->timelineTrack(), rightTrack = right->timelineTrack();
        leftTrack.trackIndex = rightTrack.trackIndex = 0;
        leftTrack.clip.startSlot = 30; leftTrack.clip.durationSlots = 60;
        rightTrack.clip.startSlot = 90; rightTrack.clip.durationSlots = 60;
        left->setTimelineTrack(leftTrack); right->setTimelineTrack(rightTrack);
        f.view.rootObject()->setProperty("viewDurationMs",
            doc->timelineSettings().timeMs(1) * (f.item("timelineTracks")->width() - 24) / 4);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        QQuickItem* leftItem = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClip"))
            if (item->property("modelData").toMap().value("id").toString() == leftTrack.clip.id) leftItem = item;
        QVERIFY(leftItem);
        QList<QQuickItem*> jointHandles;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClipJointTrim"))
            if (item->isVisible()) jointHandles.append(item);
        QCOMPARE(jointHandles.size(), 1); // A single independent overlay owns the exact centre.
        auto* joint = jointHandles.first();
        auto* normal = timelineItems(leftItem, "timelineClipTrimEnd").first();
        QQuickItem* rightItem = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClip"))
            if (item->property("modelData").toMap().value("id").toString() == rightTrack.clip.id) rightItem = item;
        QVERIFY(rightItem);
        auto* normalRight = timelineItems(rightItem, "timelineClipTrimStart").first();
        const auto viewDuration = f.view.rootObject()->property("viewDurationMs").toReal();
        for (const qreal zoom : {0.5, 1.0, 2.0}) {
            f.view.rootObject()->setProperty("viewDurationMs", viewDuration * zoom);
            QCOMPARE(joint->mapToScene({joint->width(), 0}).x() - joint->mapToScene({0, 0}).x(), qreal(jointWidthPx));
            QCOMPARE(normal->width(), qreal(normalWidth));
            QCOMPARE(normal->mapToScene({normal->width(), 0}).x(), joint->mapToScene({0, 0}).x());
            QCOMPARE(normalRight->mapToScene({0, 0}).x(), joint->mapToScene({joint->width(), 0}).x());
            QCOMPARE(normalRight->width(), qreal(normalWidth));
            QCOMPARE(joint->mapToScene({joint->width()/2, 0}).x(), leftItem->mapToScene({leftItem->width(), 0}).x());
        }
        f.view.rootObject()->setProperty("viewDurationMs", viewDuration);
        const auto origin = leftItem->mapToScene({leftItem->width(), leftItem->height()/2}).toPoint();
        const qreal width = f.timeline.clipResizeHandleWidthPx();
        const qreal jointWidth = f.timeline.clipJointResizeHandleWidthPx();
        const qreal normalOffset = (width + jointWidth) / 2;
        const qreal outsideOffset = width + jointWidth/2 + 2;
        for (const bool selectLeft : {true, false}) {
            f.timeline.selectClip((selectLeft ? leftTrack : rightTrack).clip.id);
            // Sweep across the ordinary and joint handles without pressing.
            for (const qreal offset : {-outsideOffset, -(jointWidth/2 + width - 1), -normalOffset,
                                      -(jointWidth/2 + 1), -1.0, 0.0, 1.0, jointWidth/2 + 1,
                                      normalOffset, jointWidth/2 + width - 1, outsideOffset}) {
                QTest::mouseMove(&f.view, origin + QPoint(qRound(offset), 0));
                QTRY_COMPARE(f.view.cursor().shape(), qAbs(offset) > width + jointWidth / 2 ? Qt::OpenHandCursor
                    : qAbs(offset) < jointWidth / 2 ? Qt::SplitHCursor : Qt::SizeHorCursor);
            }
        }
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        f.view.rootObject()->forceActiveFocus();
        QTest::mouseMove(&f.view, origin);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SplitHCursor);
        const auto controlKey = Qt::Key(f.view.rootObject()->property("controlKey").toInt());
        QTest::keyPress(&f.view, controlKey);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SizeHorCursor);
        QTest::keyRelease(&f.view, controlKey);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SplitHCursor);
        QCOMPARE(left->timelineTrack().toJson(), leftTrack.toJson());
        QCOMPARE(right->timelineTrack().toJson(), rightTrack.toJson());
        // Breaking contact removes the joint handle and recentres both ordinary handles.
        f.timeline.trimClip(leftTrack.clip.id, 1000, doc->timelineSettings().timeMs(89));
        QVERIFY(!joint->isVisible());
        QCOMPARE(normal->x() + normal->width()/2, leftItem->width());
        QCOMPARE(normalRight->x() + normalRight->width()/2, 0.0);
    }

    void jointResizeThresholdUsesTheCombinedDisplayedWidth_data()
    {
        QTest::addColumn<int>("leftSlots"); QTest::addColumn<int>("rightSlots");
        QTest::addColumn<int>("minimum"); QTest::addColumn<bool>("available");
        QTest::newRow("two-small-clips-at-threshold") << 9 << 9 << 36 << true;
        QTest::newRow("two-small-clips-below-threshold") << 9 << 9 << 37 << false;
        QTest::newRow("asymmetric-at-threshold") << 3 << 27 << 60 << true;
        QTest::newRow("asymmetric-below-threshold") << 3 << 27 << 61 << false;
        QTest::newRow("disabled-cutoff") << 1 << 1 << 0 << true;
        QTest::newRow("independent-resize-still-available") << 60 << 60 << 1000 << false;
    }

    void jointResizeThresholdUsesTheCombinedDisplayedWidth()
    {
        QFETCH(int, leftSlots); QFETCH(int, rightSlots); QFETCH(int, minimum); QFETCH(bool, available);
        const AppConfig previous = AppConfig::instance();
        const auto restore = qScopeGuard([&] { AppConfig::instance() = previous; });
        AppConfig::LoadOptions options; options.defaultEnvFilePath.clear();
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_CLIP_JOINT_MIN_RESIZE_WIDTH_PX", QString::number(minimum));
        QString error; QVERIFY2(AppConfig::instance().load(options, &error), qPrintable(error));
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* left = doc->addText({}, "Left");
        auto* right = doc->addText({}, "Right"); QVERIFY(left && right);
        auto leftTrack = left->timelineTrack(), rightTrack = right->timelineTrack();
        leftTrack.trackIndex = rightTrack.trackIndex = 0;
        leftTrack.clip.startSlot = 30; leftTrack.clip.durationSlots = leftSlots;
        rightTrack.clip.startSlot = 30 + leftSlots; rightTrack.clip.durationSlots = rightSlots;
        left->setTimelineTrack(leftTrack); right->setTimelineTrack(rightTrack);
        const qreal duration = doc->timelineSettings().timeMs(1) * (f.item("timelineTracks")->width() - 24) / 2;
        f.view.rootObject()->setProperty("viewDurationMs", duration);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        QQuickItem* clip = nullptr;
        QQuickItem* joint = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClip"))
            if (item->property("modelData").toMap().value("id").toString() == leftTrack.clip.id) clip = item;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClipJointTrim"))
            if (item->property("modelData").toMap().value("id").toString() == leftTrack.clip.id) joint = item;
        QVERIFY(clip && joint);
        QCOMPARE(f.timeline.clipJointMinResizeWidthPx(), minimum);
        QCOMPARE(f.timeline.clipMinResizeWidthPx(), 24);
        QCOMPARE(joint->isVisible(), available);
        const bool individual = leftSlots * 2 >= 24;
        QCOMPARE(clip->property("resizeAvailable").toBool(), individual);
        auto* normal = timelineItems(clip, "timelineClipTrimEnd").first();
        if (available && minimum > 0) {
            f.view.rootObject()->setProperty("viewDurationMs", duration * 2);
            QVERIFY(!joint->isVisible());
            QCOMPARE(normal->x() + normal->width()/2, clip->width());
            f.view.rootObject()->setProperty("viewDurationMs", duration);
            QVERIFY(joint->isVisible());
        }
        const auto from = available ? joint->mapToScene({joint->width()/2, joint->height()/2}).toPoint()
            : clip->mapToScene({clip->width() - 1, clip->height()/2}).toPoint();
        QTest::mouseMove(&f.view, from);
        QTRY_COMPARE(f.view.cursor().shape(), available ? Qt::SplitHCursor
            : individual ? Qt::SizeHorCursor : Qt::OpenHandCursor);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        QCOMPARE(clip->property("editRolling").toBool(), available);
        QCOMPARE(clip->property("editEdge").toInt(), available || individual ? 1 : 0);
        if (available && rightSlots > 1) {
            f.movePointer(from + QPoint(20, 0));
            QVERIFY(joint->isVisible());
            QVERIFY(clip->property("dragging").toBool());
            QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, from + QPoint(20, 0));
            const auto delta = qMin(10, rightSlots - 1);
            QCOMPARE(left->timelineTrack().clip.durationSlots, leftSlots + delta);
            QCOMPARE(right->timelineTrack().clip.durationSlots, rightSlots - delta);
            QCOMPARE(left->timelineTrack().clip.endSlot(), right->timelineTrack().clip.startSlot);
        } else {
            QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, from);
            QCOMPARE(left->timelineTrack().toJson(), leftTrack.toJson());
            QCOMPARE(right->timelineTrack().toJson(), rightTrack.toJson());
        }
    }

    void jointResizeSnappingIgnoresBothParticipants()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* left = doc->addText({}, "Left");
        auto* right = doc->addText({}, "Right");
        auto* other = doc->addText({}, "Other"); QVERIFY(left && right && other);
        auto leftTrack = left->timelineTrack(), rightTrack = right->timelineTrack(), otherTrack = other->timelineTrack();
        leftTrack.trackIndex = rightTrack.trackIndex = 0; otherTrack.trackIndex = 1;
        leftTrack.clip.startSlot = 30; leftTrack.clip.durationSlots = 60;
        rightTrack.clip.startSlot = 90; rightTrack.clip.durationSlots = 60;
        otherTrack.clip.startSlot = 100; otherTrack.clip.durationSlots = 30;
        rightTrack.keyframes = {{"stationary-key", 95, right->authorElementState()}};
        left->setTimelineTrack(leftTrack); right->setTimelineTrack(rightTrack); other->setTimelineTrack(otherTrack);
        const auto& grid = doc->timelineSettings();
        f.view.rootObject()->setProperty("viewDurationMs", grid.timeMs(1) * (f.item("timelineTracks")->width() - 24) / 4);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        f.timeline.seek(grid.timeMs(110));
        for (const int edge : {-1, 1}) {
            const auto id = (edge < 0 ? rightTrack : leftTrack).clip.id;
            for (const int slot : {89, 90, 91}) {
                const auto snap = f.timeline.snapTime(grid.timeMs(slot), f.scale(), id, 0, true, edge);
                QVERIFY(!snap.value("snapped").toBool());
                QCOMPARE(snap.value("timeMs").toReal(), grid.timeMs(slot));
            }
            QVERIFY(f.timeline.snapTime(3000, f.scale(), id, 0, true).value("snapped").toBool());
            const auto key = f.timeline.snapTime(grid.timeMs(94), f.scale(), id, 0, true, edge);
            QCOMPARE(key.value("targetKind").toString(), "keyframe");
            QCOMPARE(key.value("targetSlot").toLongLong(), 95);
        }
        QQuickItem* joint = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClipJointTrim"))
            if (item->isVisible()) joint = item;
        QVERIFY(joint);
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        f.timeline.selectClip(rightTrack.clip.id);
        const auto from = joint->mapToScene({joint->width()/2, joint->height()/2}).toPoint();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        f.movePointer(from + QPoint(40, 0)); // Cross the drag threshold before returning near the old junction.
        f.movePointer(from + QPoint(4, 0));
        QTest::keyPress(&f.view, Qt::Key_Shift);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
        auto* active = qvariant_cast<QQuickItem*>(f.view.rootObject()->property("activeDrag")); QVERIFY(active);
        QCOMPARE(active->property("shownEnd").toReal(), grid.timeMs(91));
        f.movePointer(from + QPoint(36, 0), Qt::ShiftModifier);
        QCOMPARE(active->property("shownEnd").toReal(), grid.timeMs(100));
        QCOMPARE(f.view.rootObject()->property("snapGuideLabel").toString(), other->displayName());
        f.movePointer(from + QPoint(76, 0), Qt::ShiftModifier);
        QCOMPARE(active->property("shownEnd").toReal(), grid.timeMs(110));
        QCOMPARE(f.view.rootObject()->property("snapGuideLabel").toString(), QStringLiteral("Playhead"));
        QCOMPARE(writes.count(), 0);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::ShiftModifier, from + QPoint(76, 0));
        QTest::keyRelease(&f.view, Qt::Key_Shift);
        QCOMPARE(left->timelineTrack().clip.endSlot(), 110);
        QCOMPARE(right->timelineTrack().clip.startSlot, 110);
        QCOMPARE(right->timelineTrack().keyframes.first().slot, 95);
        QCOMPARE(writes.count(), 1);
    }

    void rollingVideoTrimPreservesSourcesAndPublishesBothClipsTogether()
    {
        auto& residency = MediaResidencyManager::instance();
        residency.setMemorySnapshotForTesting({8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
        const auto resetMemory = qScopeGuard([&] { residency.clearMemorySnapshotForTesting(); });
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* left = doc->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {160, 90}, true, {});
        auto* right = doc->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {160, 90}, true, {});
        QVERIFY(left && right);
        QTRY_VERIFY(left->residencyReady() && right->residencyReady());
        auto leftTrack = left->timelineTrack(), rightTrack = right->timelineTrack();
        leftTrack.trackIndex = rightTrack.trackIndex = 0;
        leftTrack.clip.startSlot = 30; leftTrack.clip.durationSlots = 60; leftTrack.clip.sourceStartSlot = 10;
        rightTrack.clip.startSlot = 90; rightTrack.clip.durationSlots = 60; rightTrack.clip.sourceStartSlot = 0;
        leftTrack.keyframes = {{"left-key", 60, left->authorElementState()}};
        rightTrack.keyframes = {{"right-key", 120, right->authorElementState()}};
        left->setTimelineTrack(leftTrack); right->setTimelineTrack(rightTrack);
        const auto leftId = leftTrack.clip.id, rightId = rightTrack.clip.id;
        bool atomic = true;
        const auto observe = [&] {
            atomic &= left->timelineTrack().clip.endSlot() == right->timelineTrack().clip.startSlot;
        };
        connect(doc, &CanvasDocument::documentChanged, doc, observe);
        connect(left, &CanvasMedia::changed, doc, observe);
        connect(right, &CanvasMedia::changed, doc, observe);
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        const auto& grid = doc->timelineSettings();
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        f.timeline.selectClip(leftId);
        f.view.rootObject()->setProperty("viewDurationMs", 6000.0);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        QQuickItem* leftItem = nullptr;
        QQuickItem* rightItem = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClip")) {
            const auto id = item->property("modelData").toMap().value("id").toString();
            if (id == leftId) leftItem = item;
            if (id == rightId) rightItem = item;
        }
        QVERIFY(leftItem && rightItem);
        const auto beforeDrag = doc->serializeProjectState();
        const auto from = leftItem->mapToScene({leftItem->width() - 1, leftItem->height()/2}).toPoint();
        const auto to = from - QPoint(100, 0);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        f.movePointer(to);
        QVERIFY(leftItem->property("dragging").toBool());
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SplitHCursor);
        const qreal boundaryMs = leftItem->property("shownEnd").toReal();
        QCOMPARE(rightItem->property("shownStart").toReal(), boundaryMs);
        QCOMPARE(rightItem->property("leadingHoldMs").toReal(), 3000 - boundaryMs);
        // Control temporarily restores independent overwrite; release restores the roll.
        const auto controlKey = Qt::Key(f.view.rootObject()->property("controlKey").toInt());
        QTest::keyPress(&f.view, controlKey);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SizeHorCursor);
        QCOMPARE(rightItem->property("shownStart").toReal(), 3000.0);
        QCOMPARE(rightItem->property("leadingHoldMs").toReal(), 0.0);
        QTest::keyRelease(&f.view, controlKey);
        QTRY_COMPARE(f.view.cursor().shape(), Qt::SplitHCursor);
        QCOMPARE(rightItem->property("shownStart").toReal(), boundaryMs);
        f.movePointer(from - QPoint(600, 0));
        QCOMPARE(leftItem->property("editCursor").toInt(), int(Qt::SplitHCursor)); // Pointer is outside the window.
        QCOMPARE(grid.nearestSlot(leftItem->property("shownEnd").toReal()), 31);
        QCOMPARE(rightItem->property("shownStart").toReal(), leftItem->property("shownEnd").toReal());
        f.movePointer(from + QPoint(600, 0));
        QCOMPARE(leftItem->property("editCursor").toInt(), int(Qt::SplitHCursor));
        QCOMPARE(grid.nearestSlot(rightItem->property("shownStart").toReal()), 149);
        QCOMPARE(leftItem->property("shownEnd").toReal(), rightItem->property("shownStart").toReal());
        QCOMPARE(doc->serializeProjectState(), beforeDrag); QCOMPARE(writes.count(), 0);
        QVERIFY(QMetaObject::invokeMethod(leftItem, "cancelEdit"));
        f.movePointer(to); // A cancelled press cannot start another edit before release.
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(leftItem->property("shownEnd").toReal(), 3000.0);
        QCOMPARE(rightItem->property("shownStart").toReal(), 3000.0);
        QCOMPARE(doc->serializeProjectState(), beforeDrag); QCOMPARE(writes.count(), 0);
        for (qint64 boundary : {60, 120, 31, 149, 90}) {
            // Alternate the side holding the mouse; both sides must roll identically.
            const bool trimLeft = boundary == 60 || boundary == 31 || boundary == 90;
            f.timeline.trimClip(trimLeft ? leftId : rightId,
                grid.timeMs(trimLeft ? 30 : boundary), grid.timeMs(trimLeft ? boundary : 150), false, true);
            QVERIFY2(f.timeline.errorText().isEmpty(), qPrintable(f.timeline.errorText()));
            QVERIFY(atomic);
            QCOMPARE(left->timelineTrack().clip.startSlot, 30);
            QCOMPARE(left->timelineTrack().clip.endSlot(), boundary);
            QCOMPARE(right->timelineTrack().clip.startSlot, boundary);
            QCOMPARE(right->timelineTrack().clip.endSlot(), 150);
            QCOMPARE(left->timelineTrack().clip.sourceStartSlot.value(), 10);
            QCOMPARE(right->timelineTrack().clip.sourceStartSlot.value(), boundary - 90);
            QCOMPARE(left->timelineTrack().toJson().value("keyframes"), leftTrack.toJson().value("keyframes"));
            QCOMPARE(right->timelineTrack().toJson().value("keyframes"), rightTrack.toJson().value("keyframes"));
        }
        QCOMPARE(writes.count(), 5); QCOMPARE(doc->media().size(), 2);
        const auto saved = doc->serializeProjectState();
        const int row = doc->timelineRow(0);
        const auto previewLeft = f.timeline.previewClipEdit(rightId, 0, 5000, row, -1, 3000, 5000, row, false, {}, true);
        QCOMPARE(previewLeft.value("startMs").toReal(), grid.timeMs(31));
        const auto previewRight = f.timeline.previewClipEdit(leftId, 1000, 9000, row, 1, 1000, 3000, row, false, {}, true);
        QCOMPARE(previewRight.value("endMs").toReal(), grid.timeMs(149));
        QCOMPARE(doc->serializeProjectState(), saved); QCOMPARE(writes.count(), 5);
        doc->setEditsLocked(true);
        QVERIFY(!doc->trimTimelineClip(leftId, 30, 60, nullptr,
            CanvasDocument::PlacementMode::Avoid, CanvasDocument::TrimMode::Rolling));
        QCOMPARE(doc->serializeProjectState(), saved); QCOMPARE(writes.count(), 5);

        // A normal trim at the same junction changes only the chosen video, including its source offset.
        doc->setEditsLocked(false);
        const auto leftBefore = left->timelineTrack().toJson();
        f.timeline.trimClip(rightId, grid.timeMs(100), 5000);
        QCOMPARE(left->timelineTrack().toJson(), leftBefore);
        QCOMPARE(right->timelineTrack().clip.startSlot, 100);
        QCOMPARE(right->timelineTrack().clip.endSlot(), 150);
        QCOMPARE(right->timelineTrack().clip.sourceStartSlot.value(), 10);
        QCOMPARE(right->timelineTrack().toJson().value("keyframes"), rightTrack.toJson().value("keyframes"));
        QCOMPARE(writes.count(), 6);
    }

    void clipDragScrollsTracksInShortPanel()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        for (int i = 0; i < 8; ++i) {
            auto* media = doc->addText({}, QString::number(i)); QVERIFY(media);
            f.timeline.trimClip(media->timelineTrack().clip.id, 0, f.timeline.maxDurationMs());
        }
        auto* first = doc->media().last();
        const auto clipId = first->timelineTrack().clip.id;
        f.view.resize(1100, 200);
        QTRY_COMPARE(f.view.rootObject()->height(), 200.0);
        auto* viewport = f.item("timelineClipViewport"); QVERIFY(viewport);
        const auto keyY = f.item("timelineKeyframeTrack")->mapToScene({0, 0}).y();
        QQuickItem* target = nullptr;
        for (auto* item : timelineItems(f.view.rootObject(), "timelineClip"))
            if (item->property("modelData").toMap().value("id").toString() == clipId) target = item;
        QVERIFY(target);
        viewport->setProperty("contentY", first->timelineTrack().trackIndex * f.timeline.clipTrackHeightPx());
        const qreal initialScroll = viewport->property("contentY").toReal();
        const auto modifier = Qt::KeyboardModifiers(f.view.rootObject()->property("controlModifier").toInt());
        const auto from = target->mapToScene({40, target->height()/2}).toPoint();
        const auto edge = viewport->mapToScene({60, viewport->height() - 3}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, modifier, from);
        f.movePointer(edge, modifier);
        QTest::qWait(200);
        const qreal distance = viewport->property("contentY").toReal() - initialScroll;
        QVERIFY(distance > 0);
        QVERIFY(distance < f.timeline.clipTrackHeightPx()); // Less than one track in 200 ms.
        QTRY_VERIFY(viewport->property("contentY").toReal() >= initialScroll + f.timeline.clipTrackHeightPx());
        const int destination = target->property("previewTrack").toInt();
        QVERIFY(destination >= 2);
        QCOMPARE(f.item("timelineKeyframeTrack")->mapToScene({0, 0}).y(), keyY);
        const int storedDestination = doc->timelineTrackAtRow(destination);
        QTest::mouseRelease(&f.view, Qt::LeftButton, modifier, edge);
        QCOMPARE(first->timelineTrack().trackIndex, storedDestination);
        QCOMPARE(f.timeline.positionMs(), 0.0);
        QCOMPARE(doc->media().size(), 7); // The full-length destination was overwritten.
    }

    void newClipsFitAtTimelineEnd()
    {
        CanvasDocument doc;
        doc.setMediaResidencySuspended(true);
        SceneTimeline::SceneSettings grid;
        grid.maxDurationMs = 2000;
        grid.slotsPerSecond = 60;
        QVERIFY(doc.setTimelineSettings(grid));
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto path = temporary.filePath("image.png");
        QImage image(64, 64, QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(path));
        const auto create = [&](const QString& type) {
            if (type == "text") return doc.addText({}, "Last slot");
            return doc.addPreparedFile(type == "video" ? QString::fromUtf8(TEST_VIDEO_FILE) : path,
                                       {64, 64}, type == "video", {}, 5000);
        };
        doc.setTimelinePosition(grid.timeMs(grid.maxSlot() - 1));
        for (const QString type : {"text", "image", "video"}) {
            auto* media = create(type); QVERIFY(media);
            const auto clip = media->timelineTrack().clip;
            QCOMPARE(clip.startSlot, grid.maxSlot() - 1);
            QCOMPARE(clip.durationSlots, 1);
            QVERIFY(media->clipActive());
            if (type == "video") QCOMPARE(clip.sourceStartSlot.value(), 0);
        }
        doc.setTimelinePosition(grid.timeMs(grid.maxSlot()));
        const auto saved = doc.serializeProjectState();
        for (const QString type : {"text", "image", "video"}) QVERIFY(!create(type));
        QVERIFY(doc.queueFileImport(path, {}).isEmpty());
        QCOMPARE(doc.serializeProjectState(), saved);
    }

    void imagesReceiveDefaultLengthClipsEvenAfterStop()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc=f.host->document();
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto path=temporary.filePath("image.png");
        QImage image(64,64,QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(path));
        f.timeline.seek(1000); f.timeline.placeStop(); f.timeline.seek(2000);
        auto* media=doc->addPreparedFile(path,{64,64},false,{}); QVERIFY(media);
        const auto clip=media->timelineTrack().clip;
        QCOMPARE(clip.startSlot,60);
        QCOMPARE(clip.durationSlots,AppConfig::instance().timelineDefaultClipDurationSlots());
        QVERIFY(!clip.sourceStartSlot); QVERIFY(media->clipActive());
        f.timeline.seek(0); QVERIFY(!media->clipActive());
        f.timeline.seek(doc->timelineSettings().timeMs(clip.endSlot())); QVERIFY(!media->clipActive());
        f.timeline.seek(2000);
        doc->select(media->mediaId()); QCOMPARE(f.timeline.clips().size(),1);
        f.timeline.trimClip(clip.id,3000,4000); QVERIFY(!media->clipActive());
        f.timeline.seek(3000); QVERIFY(media->clipActive());
        f.timeline.seek(4000); QVERIFY(!media->clipActive());
    }

    void videoExtensionsShowHoldRegionsDuringResize()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc=f.host->document();
        f.timeline.seek(2000);
        auto* media=doc->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE),{160,90},true,{});
        QVERIFY(media); QTRY_VERIFY(media->residencyReady() && media->timelineTrack().clip.durationSlots > 0);
        doc->select(media->mediaId());
        const auto clip=media->timelineTrack().clip;
        QCOMPARE(clip.startSlot, 60);
        QCOMPARE(clip.sourceStartSlot.value(), 0);
        QVERIFY(media->clipActive());
        f.timeline.moveClip(clip.id,2000);
        const qreal end=doc->timelineSettings().timeMs(media->timelineTrack().clip.endSlot());
        f.timeline.trimClip(clip.id,1000,end+2000);
        auto* item=f.item("timelineClip"); QVERIFY(item);
        QCOMPARE(item->property("leadingHoldMs").toDouble(),1000.0);
        QCOMPARE(item->property("trailingHoldMs").toDouble(),
            std::max(0.0, item->property("shownSourceIn").toDouble() + item->property("shownDuration").toDouble() - media->sourceDurationMs()));
        for (qreal zoom : {20000.0, 40000.0, 80000.0}) {
            f.view.rootObject()->setProperty("viewDurationMs", zoom);
            QCOMPARE(f.item("timelineClipLeadingHold")->width(), 1000.0 * f.scale());
            QCOMPARE(f.item("timelineClipTrailingHold")->width(),
                item->property("trailingHoldMs").toDouble() * f.scale());
        }
        f.view.rootObject()->setProperty("viewDurationMs", 40000.0);
        const auto screenshot=qEnvironmentVariable("MOUFFETTE_CLIP_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(f.view.grabWindow().save(screenshot));
        }
        // A drag preview uses the same signed source mapping, before committing.
        const auto saved=doc->serializeSceneState();
        item->setProperty("initialStart",1000.0); item->setProperty("initialEnd",end+2000);
        item->setProperty("previewStart",0.0); item->setProperty("previewEnd",end+2000);
        item->setProperty("editEdge",-1); item->setProperty("dragging",true);
        QCOMPARE(item->property("leadingHoldMs").toDouble(),2000.0);
        QCOMPARE(doc->serializeSceneState(),saved);
        item->setProperty("dragging",false);
        f.timeline.selectClip(clip.id); f.timeline.copySelected(); f.timeline.seek(30000); f.timeline.paste();
        QCOMPARE(media->timelineTrack().clip.sourceStartSlot.value(),-30);
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

    void activeSeeksKeepRunningFromTheNewPosition()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host); host->setProjectEditingEnabled(true);
        auto* doc = host->document();
        QVERIFY(doc->addText({}, "Seek while playing"));
        auto settings = doc->timelineSettings();
        settings.stopSlot = settings.nearestSlot(10000);
        QVERIFY(doc->setTimelineSettings(settings));
        TimelineController timeline; timeline.setHost(host.get());
        const auto saved = doc->serializeProjectState();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        timeline.togglePlayback();
        QTRY_VERIFY(host->timelinePlaying());
        QSignalSpy locks(doc, &CanvasDocument::editsLockedChanged);
        for (qreal target : {6000.0, 1000.0, 4000.0}) {
            timeline.seek(target);
            QVERIFY(timeline.playing());
            QVERIFY(doc->editsLocked());
            QCOMPARE(doc->timelinePositionMs(), target);
            QTRY_VERIFY(doc->timelinePositionMs() > target + 50);
            QVERIFY(doc->timelinePositionMs() < target + 1000);
        }
        timeline.goToStart();
        QVERIFY(timeline.playing());
        QCOMPARE(timeline.positionSlot(), 0);
        QTRY_VERIFY(doc->timelinePositionMs() > 50);
        QVERIFY(doc->timelinePositionMs() < 1000);
        QCOMPARE(locks.count(), 0);
        timeline.goToEnd();
        QVERIFY(!timeline.playing());
        QVERIFY(!doc->editsLocked());
        QCOMPARE(doc->timelinePositionMs(), 10000);
        timeline.seek(2000);
        QTest::qWait(80);
        QVERIFY(!timeline.playing());
        QCOMPARE(doc->timelinePositionMs(), 2000);
        QCOMPARE(writes.count(), 0);
        QCOMPARE(doc->serializeProjectState(), saved);
    }

    void rulerSeekingPreservesPlaybackState_data()
    {
        QTest::addColumn<bool>("playing");
        QTest::newRow("paused") << false;
        QTest::newRow("playing") << true;
    }

    void rulerSeekingPreservesPlaybackState()
    {
        QFETCH(bool, playing);
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        if (playing) {
            f.timeline.togglePlayback();
            QTRY_VERIFY(f.host->timelinePlaying());
        }
        auto* tracks = f.item("timelineTracks"); QVERIFY(tracks);
        const auto pointAt = [&](int x) { return tracks->mapToScene({qreal(x), 10}).toPoint(); };
        const qreal clicked = f.timeline.gridTime(f.timeAt(300));
        QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier, pointAt(300));
        QCOMPARE(f.timeline.playing(), playing);
        QVERIFY(qAbs(f.timeline.positionMs() - clicked) < 100);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, pointAt(350));
        const qreal pressed = f.timeline.gridTime(f.timeAt(350));
        QTest::qWait(120);
        QCOMPARE(f.host->timelinePositionMs(), pressed);
        QVERIFY(!f.host->timelinePlaying());
        f.movePointer(pointAt(150));
        const qreal dragged = f.timeline.gridTime(f.timeAt(150));
        QTest::qWait(120);
        QCOMPARE(f.host->timelinePositionMs(), dragged);
        QCOMPARE(f.timeline.playing(), playing);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, pointAt(150));
        QCOMPARE(f.timeline.playing(), playing);
        if (playing) {
            QTRY_VERIFY(f.timeline.positionMs() > dragged + 100);
            QVERIFY(f.timeline.positionMs() < dragged + 1000);
            f.timeline.togglePlayback();
        } else {
            QTest::qWait(100);
            QCOMPARE(f.timeline.positionMs(), dragged);
        }
    }

    void cancelledScrubDoesNotResumePlayback_data()
    {
        QTest::addColumn<bool>("explicitPause");
        QTest::newRow("panel-collapsed") << false;
        QTest::newRow("pause-while-held") << true;
    }

    void cancelledScrubDoesNotResumePlayback()
    {
        QFETCH(bool, explicitPause);
        TimelineFixture f; QVERIFY(f.initialize());
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        f.timeline.togglePlayback();
        QTRY_VERIFY(f.host->timelinePlaying());
        auto* tracks = f.item("timelineTracks"); QVERIFY(tracks);
        const auto point = tracks->mapToScene({300, 10}).toPoint();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(!f.host->timelinePlaying());
        QVERIFY(f.timeline.playing());
        if (explicitPause) f.timeline.togglePlayback();
        else f.view.rootObject()->setProperty("expanded", false);
        QVERIFY(!f.timeline.playing());
        QVERIFY(!f.host->document()->editsLocked());
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::NoModifier, point);
        const auto stoppedAt = f.host->timelinePositionMs();
        QTest::qWait(120);
        QVERIFY(!f.timeline.playing());
        QCOMPARE(f.host->timelinePositionMs(), stoppedAt);
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
        const auto pasteSlot = timeline.positionSlot();
        timeline.paste();
        QCOMPARE(timeline.positionSlot(), pasteSlot);
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
        auto track = b->timelineTrack();
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

    void playheadSnapUsesThresholdAndCanBeExcluded()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        TimelineController timeline; timeline.setHost(host.get());
        timeline.seek(5000);
        for (qreal duration : {0.0, 2000.0}) {
            const qreal start = 5000 - duration;
            const auto snap = timeline.snapTime(start + 5, 1, {}, duration, true);
            QVERIFY(snap.value("snapped").toBool());
            QCOMPARE(snap.value("timeMs").toReal(), start);
            QCOMPARE(snap.value("targetTimeMs").toReal(), 5000.0);
            QCOMPARE(snap.value("mediaName").toString(), QStringLiteral("Playhead"));
            QVERIFY(!timeline.snapTime(start + timeline.snapDistancePx() + 1, 1, {}, duration, true)
                .value("snapped").toBool());
            QVERIFY(!timeline.snapTime(start + 5, 1, {}, duration).value("snapped").toBool());
        }
        // The nearest target still wins when another clip is closer than the head.
        auto* media = host->document()->addText({}, "Other target"); QVERIFY(media);
        auto track = media->timelineTrack(); track.clip.startSlot = 153;
        media->setTimelineTrack(track);
        QCOMPARE(timeline.snapTime(5090, 0.1, {}, 0, true).value("targetTimeMs").toReal(), 5100.0);
        timeline.seek(5100);
        const auto coincident = timeline.snapTime(5105, 1, {}, 0, true);
        QCOMPARE(coincident.value("targetKind").toString(), QStringLiteral("playhead"));
        QCOMPARE(coincident.value("mediaName").toString(), QStringLiteral("Playhead"));
        timeline.seek(0);
        QCOMPARE(timeline.snapTime(5, 1, {}, 2000, true).value("timeMs").toReal(), 0.0);
    }

    void clipShiftSnapsToPlayhead_data()
    {
        QTest::addColumn<int>("edge");
        QTest::addColumn<bool>("snapEnd");
        QTest::addColumn<int>("durationSlots");
        QTest::addColumn<int>("rate");
        for (int rate : {24, 30, 60, 240}) for (int duration : {59, 60, 61, 91}) {
            const auto suffix = QString("%1-slots-%2-fps").arg(duration).arg(rate);
            QTest::newRow(qPrintable("move-start-" + suffix)) << 0 << false << duration << rate;
            QTest::newRow(qPrintable("move-end-" + suffix)) << 0 << true << duration << rate;
            QTest::newRow(qPrintable("trim-start-" + suffix)) << -1 << false << duration << rate;
            QTest::newRow(qPrintable("trim-end-" + suffix)) << 1 << true << duration << rate;
        }
    }

    void clipShiftSnapsToPlayhead()
    {
        QFETCH(int, edge); QFETCH(bool, snapEnd);
        QFETCH(int, durationSlots);
        QFETCH(int, rate);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto settings = doc->timelineSettings(); settings.slotsPerSecond = rate;
        QVERIFY(doc->setTimelineSettings(settings));
        auto* media = doc->addText({}, "Clip"); QVERIFY(media);
        auto track = media->timelineTrack();
        track.clip.startSlot = 30; track.clip.durationSlots = durationSlots;
        media->setTimelineTrack(track);
        const auto& grid = doc->timelineSettings();
        const qreal initialStart = grid.timeMs(track.clip.startSlot);
        const qreal initialEnd = grid.timeMs(track.clip.endSlot());
        const qreal head = grid.timeMs(edge < 0 ? 60 : edge > 0 ? 151 : 157);
        f.timeline.seek(head);
        f.view.rootObject()->setProperty("viewDurationMs", grid.timeMs(300));
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        auto* handle = edge == 0 ? clip : timelineItems(clip,
            edge < 0 ? "timelineClipTrimStart" : "timelineClipTrimEnd").first();
        const auto from = handle->mapToScene({handle->width()/2, handle->height()/2}).toPoint();
        const qreal desired = head - (edge == 0 && snapEnd ? grid.timeMs(durationSlots) : 0);
        const auto to = from + QPoint(qRound((desired - (edge > 0 ? initialEnd : initialStart)) * f.scale()) + 5, 0);
        const auto saved = doc->serializeProjectState();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::NoModifier, from);
        f.movePointer(to);
        QVERIFY(clip->property("dragging").toBool());
        const auto snappedEdge = snapEnd ? "shownEnd" : "shownStart";
        const qreal unsnapped = clip->property(snappedEdge).toReal();
        QVERIFY(unsnapped != head);
        QTest::keyPress(&f.view, Qt::Key_Shift);
        QCOMPARE(clip->property(snappedEdge).toReal(), head);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), head);
        QCOMPARE(f.view.rootObject()->property("snapGuideLabel").toString(), QStringLiteral("Playhead"));
        QVERIFY(f.item("timelineSnapGuide")->isVisible());
        QVERIFY(f.item("timelineSnapGuideLabel")->isVisible());
        QCOMPARE(f.item("timelineSnapGuideLabel")->property("text").toString(), QStringLiteral("Playhead"));
        QCOMPARE(f.timeline.positionMs(), head);
        QCOMPARE(doc->serializeProjectState(), saved); QCOMPARE(writes.count(), 0);
        QTest::keyRelease(&f.view, Qt::Key_Shift);
        QCOMPARE(clip->property(snappedEdge).toReal(), unsnapped);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
        QTest::keyPress(&f.view, Qt::Key_Shift);
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::ShiftModifier, to);
        QTest::keyRelease(&f.view, Qt::Key_Shift);
        const auto& result = media->timelineTrack().clip;
        QCOMPARE(doc->timelineSettings().timeMs(snapEnd ? result.endSlot() : result.startSlot), head);
        if (edge == 0) QCOMPARE(result.durationSlots, durationSlots);
        else if (edge < 0) QCOMPARE(result.endSlot(), track.clip.endSlot());
        else QCOMPARE(result.startSlot, 30);
        QCOMPARE(f.timeline.positionMs(), head); QCOMPARE(writes.count(), 1);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
    }

    void snapGuideUsesTheResolvedEdgeAndPlacementLimits()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* moving = doc->addText({}, "Moving");
        auto* obstacle = doc->addText({}, "Obstacle");
        QVERIFY(moving && obstacle);
        auto track = moving->timelineTrack();
        track.trackIndex = 0; track.clip.startSlot = 30; track.clip.durationSlots = 61;
        moving->setTimelineTrack(track);
        auto other = obstacle->timelineTrack();
        const auto placeObstacle = [&](int row, qint64 start, qint64 end) {
            other.trackIndex = row; other.clip.startSlot = start; other.clip.durationSlots = end - start;
            obstacle->setTimelineTrack(other);
        };
        const auto& grid = doc->timelineSettings();
        bool readOnlyPreviews = true;
        const auto preview = [&](qint64 target, int edge, bool snapEnd, int row = 0, bool overwrite = false, bool rolling = false) {
            f.timeline.seek(grid.timeMs(target));
            const auto saved = doc->serializeProjectState();
            QSignalSpy writes(doc, &CanvasDocument::documentChanged);
            const qint64 wantedSlot = target - (edge == 0 && snapEnd ? track.clip.durationSlots : 0);
            const auto snap = f.timeline.snapTime(grid.timeMs(wantedSlot) + 1, 1, track.clip.id,
                edge == 0 ? grid.timeMs(track.clip.durationSlots) : 0, true);
            const auto result = f.timeline.previewClipEdit(track.clip.id,
                edge > 0 ? grid.timeMs(30) : snap.value("timeMs").toReal(),
                edge < 0 ? grid.timeMs(91) : edge > 0 ? snap.value("timeMs").toReal()
                    : snap.value("timeMs").toReal() + grid.timeMs(61),
                doc->timelineRow(row), edge, grid.timeMs(30), grid.timeMs(91), doc->timelineRow(0), overwrite, snap, rolling);
            readOnlyPreviews &= doc->serializeProjectState() == saved && writes.count() == 0;
            return result;
        };
        placeObstacle(1, 0, 300);
        auto result = preview(30, 0, false, 1);
        QCOMPARE(result.value("row").toInt(), doc->timelineRow(0)); // Rejected row, still aligned in time.
        QVERIFY(result.value("snap").toMap().value("snapped").toBool());

        placeObstacle(0, 100, 180);
        result = preview(157, 0, false);
        QCOMPARE(result.value("endMs").toReal(), grid.timeMs(100));
        QVERIFY(result.value("snap").toMap().isEmpty());
        // Only the opposite edge lands on the target; this is not the requested snap.
        result = preview(100, 0, false);
        QCOMPARE(result.value("endMs").toReal(), grid.timeMs(100));
        QVERIFY(result.value("snap").toMap().isEmpty());
        QVERIFY(preview(157, 0, false, 0, true).value("snap").toMap().value("snapped").toBool());
        QVERIFY(preview(100, 0, true).value("snap").toMap().value("snapped").toBool());
        QVERIFY(preview(30, 1, false).value("snap").toMap().isEmpty()); // Minimum duration.
        QVERIFY(preview(91, -1, false).value("snap").toMap().isEmpty());

        placeObstacle(0, 91, 130);
        result = preview(120, 1, false);
        QCOMPARE(result.value("endMs").toReal(), grid.timeMs(91));
        QVERIFY(result.value("adjacentClip").toMap().isEmpty());
        QVERIFY(result.value("snap").toMap().isEmpty());
        result = preview(120, 1, false, 0, false, true);
        QVERIFY(result.value("snap").toMap().value("snapped").toBool());
        QCOMPARE(result.value("adjacentClip").toMap().value("startMs").toReal(), grid.timeMs(120));
        result = preview(157, 1, false, 0, false, true);
        QCOMPARE(result.value("endMs").toReal(), grid.timeMs(129));
        QVERIFY(result.value("snap").toMap().isEmpty());
        QVERIFY(readOnlyPreviews);
    }

    void constrainedClipDoesNotShowAnUnreachedSnap_data()
    {
        QTest::addColumn<int>("edge");
        QTest::newRow("move-collision") << 0;
        QTest::newRow("minimum-duration-start") << -1;
        QTest::newRow("minimum-duration-end") << 1;
    }

    void constrainedClipDoesNotShowAnUnreachedSnap()
    {
        QFETCH(int, edge);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* media = doc->addText({}, "Clip"); QVERIFY(media);
        auto track = media->timelineTrack();
        track.trackIndex = 0; track.clip.startSlot = 30; track.clip.durationSlots = 61;
        media->setTimelineTrack(track);
        if (edge == 0) {
            auto* obstacle = doc->addText({}, "Obstacle"); QVERIFY(obstacle);
            auto other = obstacle->timelineTrack(); other.trackIndex = 0;
            other.clip.startSlot = 100; other.clip.durationSlots = 80;
            obstacle->setTimelineTrack(other);
        }
        const auto& grid = doc->timelineSettings();
        const qreal head = grid.timeMs(edge < 0 ? 91 : edge > 0 ? 30 : 157);
        f.timeline.seek(head);
        f.view.rootObject()->setProperty("viewDurationMs", 10000.0);
        f.item("timelineClipViewport")->setProperty("contentY", 0.0);
        auto* clip = f.item("timelineClip"); QVERIFY(clip);
        auto* handle = edge == 0 ? clip : timelineItems(clip,
            edge < 0 ? "timelineClipTrimStart" : "timelineClipTrimEnd").first();
        const auto from = handle->mapToScene({handle->width()/2, handle->height()/2}).toPoint();
        const auto to = from + QPoint(qRound((head - grid.timeMs(edge > 0 ? 91 : 30)) * f.scale()) + 5, 0);
        const auto saved = doc->serializeProjectState();
        QTest::mousePress(&f.view, Qt::LeftButton, Qt::ShiftModifier, from);
        f.movePointer(to, Qt::ShiftModifier);
        QVERIFY(clip->property("dragging").toBool());
        QVERIFY(clip->property(edge > 0 ? "shownEnd" : "shownStart").toReal() != head);
        QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
        QVERIFY(!f.item("timelineSnapGuideLabel")->isVisible());
        if (edge == 0) {
            const auto controlKey = Qt::Key(f.view.rootObject()->property("controlKey").toInt());
            QTest::keyPress(&f.view, controlKey, Qt::ShiftModifier);
            QCOMPARE(clip->property("shownStart").toReal(), head);
            QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), head);
            QVERIFY(f.item("timelineSnapGuideLabel")->isVisible());
            QTest::keyRelease(&f.view, controlKey, Qt::ShiftModifier);
            QCOMPARE(f.view.rootObject()->property("snapGuideMs").toReal(), -1.0);
        }
        QVERIFY(QMetaObject::invokeMethod(clip, "cancelEdit"));
        QTest::mouseRelease(&f.view, Qt::LeftButton, Qt::ShiftModifier, to);
        QTest::keyRelease(&f.view, Qt::Key_Shift);
        QVERIFY(!f.item("timelineSnapGuideLabel")->isVisible());
        QCOMPARE(doc->serializeProjectState(), saved);
    }

    void snapGuideLabelStaysInsideTheViewport_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("rightEdge");
        for (const auto& kind : {QStringLiteral("playhead"), QStringLiteral("clip"), QStringLiteral("keyframe")})
            for (bool right : {false, true})
                QTest::newRow(qPrintable(kind + (right ? "-right" : "-left"))) << kind << right;
    }

    void snapGuideLabelStaysInsideTheViewport()
    {
        QFETCH(QString, kind); QFETCH(bool, rightEdge);
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* target = doc->addText({}, "<b>A very long target name & literal markup</b>"); QVERIFY(target);
        f.view.rootObject()->setProperty("viewDurationMs", 10000.0);
        auto* tracks = f.item("timelineTracks");
        tracks->setProperty("contentX", 400.0);
        const auto& grid = doc->timelineSettings();
        const qint64 targetSlot = grid.nearestSlot(f.timeAt(rightEdge ? tracks->width() - 6 : 6));
        const qreal targetMs = grid.timeMs(targetSlot);
        auto track = target->timelineTrack(); track.clip.startSlot = 0; track.clip.durationSlots = 30;
        if (kind == "clip") track.clip.startSlot = targetSlot;
        if (kind == "keyframe") track.keyframes = {{"target-key", targetSlot, target->authorElementState()}};
        target->setTimelineTrack(track);
        f.timeline.seek(kind == "playhead" ? targetMs : 0);
        const auto snap = f.timeline.snapTime(targetMs + 1, f.scale(), {}, 0, kind == "playhead");
        QCOMPARE(snap.value("targetKind").toString(), kind);
        QVERIFY(QMetaObject::invokeMethod(f.view.rootObject(), "showSnapGuide", Q_ARG(QVariant, snap)));
        auto* guide = f.item("timelineSnapGuide"); auto* label = f.item("timelineSnapGuideLabel");
        QVERIFY(guide && label); QVERIFY(guide->isVisible()); QVERIFY(label->isVisible());
        QCOMPARE(label->property("text").toString(), kind == "playhead" ? QStringLiteral("Playhead") : target->displayName());
        QCOMPARE(label->property("textFormat").toInt(), 0); // Text.PlainText, including names containing markup.
        const qreal labelX = label->mapToItem(tracks, {0, 0}).x();
        QVERIFY(labelX >= 3.99);
        QVERIFY(labelX + label->width() <= tracks->width() - 3.99);
        const auto screenshot = qEnvironmentVariable("MOUFFETTE_SNAP_SCREENSHOT");
        if (!screenshot.isEmpty() && rightEdge && kind == "playhead") {
            QTest::qWait(30);
            QVERIFY(f.view.grabWindow().save(screenshot));
        }
        // A target scrolled out of view must not leave a detached name on screen.
        tracks->setProperty("contentX", guide->x() + 10);
        QVERIFY(!guide->isVisible()); QVERIFY(!label->isVisible());
        QVERIFY(QMetaObject::invokeMethod(f.view.rootObject(), "endDrag"));
        QVERIFY(f.view.rootObject()->property("snapGuide").isNull());
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
        mime->setData("application/x-mouffette-timeline-v6", QJsonDocument(QJsonObject{
            {"mediaId", media->mediaId()}, {"projectId", host->document()->projectId()},
            {"kind", "keyframe"}, {"state", wrongType.toJson()}}).toJson());
        QGuiApplication::clipboard()->setMimeData(mime);
        timeline.paste();
        QVERIFY(media->timelineTrack().keyframes.isEmpty());
        QVERIFY(!timeline.errorText().isEmpty());
        QCOMPARE(media->text(), QStringLiteral("Keep text"));
    }

    void repeatedClipPastesAdvanceTheHeadAndStayAdjacent()
    {
        TimelineFixture f; QVERIFY(f.initialize());
        auto* doc = f.host->document();
        auto* original = doc->addText({}, "Paste me"); QVERIFY(original);
        auto track = original->timelineTrack();
        track.clip.startSlot = 31; track.clip.durationSlots = 61;
        original->setTimelineTrack(track);
        f.timeline.selectClip(track.clip.id); f.timeline.copySelected();
        const auto& grid = doc->timelineSettings();
        f.timeline.seek(grid.timeMs(200));
        f.view.rootObject()->setProperty("viewDurationMs", grid.timeMs(90));
        f.view.requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&f.view));
        f.view.rootObject()->forceActiveFocus();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        for (int i = 0; i < 3; ++i) {
            const qint64 pasteStart = f.timeline.positionSlot();
            QVERIFY(f.timeline.canPaste());
            if (i == 1) QTest::keyClick(&f.view, Qt::Key_V, Qt::ControlModifier);
            else {
                auto* button = f.item("timelinePaste"); QVERIFY(button);
                QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier,
                    button->mapToScene({button->width()/2, button->height()/2}).toPoint());
            }
            QCOMPARE(doc->media().size(), i + 2);
            auto* pasted = doc->primarySelectedMedia(); QVERIFY(pasted && pasted != original);
            QCOMPARE(pasted->timelineTrack().clip.startSlot, pasteStart);
            QCOMPARE(pasted->timelineTrack().clip.durationSlots, 61);
            QCOMPARE(f.timeline.positionSlot(), pasteStart + 61);
            QCOMPARE(f.timeline.positionSlot(), pasted->timelineTrack().clip.endSlot());
            QCOMPARE(f.timeline.selectedClipId(), pasted->timelineTrack().clip.id);
            QCOMPARE(writes.count(), i + 1); // Seeking adds no persistent edit.
            auto* viewport = f.item("timelineTracks");
            const auto x = f.item("timelinePlayhead")->mapToItem(viewport, {0, 0}).x();
            QVERIFY(x >= 0 && x < viewport->width());
        }
        QCOMPARE(original->timelineTrack().toJson(), track.toJson());
    }

    void failedClipPasteKeepsTheHeadInPlace()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create()); QVERIFY(host);
        host->setProjectEditingEnabled(true);
        TimelineController timeline; timeline.setHost(host.get());
        auto* doc = host->document();
        auto* media = doc->addText({}, "Clip"); QVERIFY(media);
        timeline.selectClip(media->timelineTrack().clip.id); timeline.copySelected();
        timeline.seek(7000);
        auto copy = QJsonDocument::fromJson(QGuiApplication::clipboard()->mimeData()
            ->data("application/x-mouffette-timeline-v6")).object();
        auto snapshot = copy.value("media").toObject();
        auto track = snapshot.value("timeline").toObject();
        auto clip = track.value("clip").toObject(); clip.insert("durationSlots", 0);
        track.insert("clip", clip); snapshot.insert("timeline", track); copy.insert("media", snapshot);
        auto* mime = new QMimeData;
        mime->setData("application/x-mouffette-timeline-v6", QJsonDocument(copy).toJson());
        QGuiApplication::clipboard()->setMimeData(mime);
        const auto saved = doc->serializeProjectState();
        QSignalSpy writes(doc, &CanvasDocument::documentChanged);
        QSignalSpy reveal(&timeline, &TimelineController::revealPlayhead);
        QVERIFY(timeline.canPaste()); timeline.paste();
        QVERIFY(!timeline.errorText().isEmpty());
        QCOMPARE(timeline.positionMs(), 7000.0);
        QCOMPARE(doc->serializeProjectState(), saved);
        QCOMPARE(writes.count(), 0); QCOMPARE(reveal.count(), 0);
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
        track.clip = {QStringLiteral("original"), 0, 0, 60};
        media->setTimelineTrack(track);
        TimelineController timeline;
        timeline.setHost(host.get());
        timeline.placeKeyframe();
        const auto key = media->timelineTrack().keyframes.first().id;
        timeline.selectClip(QStringLiteral("original"));
        timeline.copySelected();
        timeline.seek(1750);
        timeline.paste();
        QCOMPARE(host->document()->media().size(), 2);
        QCOMPARE(media->timelineTrack().clip.sourceEndSlot(), 53);
        auto* pasted = host->document()->primarySelectedMedia(); QVERIFY(pasted && pasted != media);
        QCOMPARE(pasted->timelineTrack().clip.durationSlots, 22);
        QCOMPARE(pasted->timelineTrack().clip.endSlot(), 75);
        QCOMPARE(timeline.positionSlot(), 75);
        QVERIFY(!timeline.canPaste());
        QCOMPARE(pasted->timelineTrack().keyframes.first().slot, 0);
        QVERIFY(pasted->timelineTrack().keyframes.first().id != key);
        QCOMPARE(media->timelineTrack().keyframes.first().id, key);
        QCOMPARE(media->timelineTrack().keyframes.first().slot, 0);
        host.reset();
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }

    void firstClipResizePreservesTheOppositeEdge_data()
    {
        QTest::addColumn<QString>("mediaType");
        QTest::addColumn<bool>("trimStart");
        for (const auto& type : {QStringLiteral("text"), QStringLiteral("image"), QStringLiteral("video")}) {
            QTest::newRow(qPrintable(type + "-start")) << type << true;
            QTest::newRow(qPrintable(type + "-end")) << type << false;
        }
    }

    void firstClipResizePreservesTheOppositeEdge()
    {
        QFETCH(QString, mediaType);
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
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto imagePath = temporary.filePath("clip.png");
        QImage image(160, 90, QImage::Format_RGB32); image.fill(Qt::cyan);
        QVERIFY(image.save(imagePath));
        auto* media = mediaType == "text" ? host->document()->addText({}, "Clip")
            : host->document()->addPreparedFile(mediaType == "video"
                ? QString::fromUtf8(TEST_VIDEO_FILE) : imagePath,
                {160, 90}, mediaType == "video", {});
        QVERIFY(media);
        host->document()->select(media->mediaId());
        const auto metadataDuration = media->sourceDurationMs();
        QTRY_VERIFY(media->residencyReady() && media->timelineTrack().clip.durationSlots > 0);
        const auto original = media->timelineTrack().clip;
        QVERIFY(original.durationSlots > 1);
        const auto& grid = host->document()->timelineSettings();
        QVERIFY(!publishedClips.isEmpty());
        for (const auto& row : publishedClips) {
            const auto duration = row.value("actualSourceDurationMs").toLongLong();
            QVERIFY(duration == metadataDuration || duration == media->sourceDurationMs());
            QCOMPARE(row.value("isVideo").toBool(), mediaType == "video");
        }
        QCOMPARE(timeline.clips().first().toMap().value("actualSourceDurationMs").toLongLong(), media->sourceDurationMs());
        QCOMPARE(timeline.clipModel()->index(0, 0).data(Qt::UserRole).toMap().value("actualSourceDurationMs").toLongLong(), media->sourceDurationMs());
        view.rootObject()->setProperty("viewDurationMs", qMax(mediaType == "video" ? 40000.0 : 2000.0,
                                                            grid.timeMs(original.endSlot())));
        const auto findItem = [&](const QString& name) {
            QQuickItem* result = nullptr;
            auto visit = [&](auto&& self, QQuickItem* item) -> void {
                if (item->objectName() == name) result = item;
                for (auto* child : item->childItems()) self(self, child);
            };
            visit(visit, view.rootObject());
            return result;
        };
        QQuickItem* clip = findItem("timelineClip");
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
        QCOMPARE(host->document()->media().size(), 1);
        const auto result = media->timelineTrack().clip;
        QCOMPARE(result.startSlot, expectedStart);
        QCOMPARE(result.endSlot(), expectedEnd);
        if (original.sourceStartSlot)
            QCOMPARE(result.sourceStartSlot, *original.sourceStartSlot + (trimStart ? expectedDelta : 0));
        else QVERIFY(!result.sourceStartSlot);

        // Re-extend the same edge to the source boundary, including its final
        // compensated slot. The first gesture must not lose that source range.
        handle = findItem(trimStart ? "timelineClipTrimStart" : "timelineClipTrimEnd");
        QVERIFY(handle);
        const QPoint extendFrom = handle->mapToScene({handle->width()/2, handle->height()/2}).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, extendFrom);
        QTest::mouseMove(&view, press, 20);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, press);
        const auto extended = media->timelineTrack().clip;
        QCOMPARE(extended.startSlot, original.startSlot);
        const qint64 expectedExtendedEnd = trimStart ? original.endSlot()
            : grid.nearestSlot(grid.timeMs(result.endSlot())
                + (press.x() - extendFrom.x()) / view.rootObject()->property("pixelsPerMs").toDouble());
        QCOMPARE(extended.endSlot(),expectedExtendedEnd);
        QCOMPARE(extended.sourceStartSlot, original.sourceStartSlot);
    }

    void productionTimelineQmlLoadsAndCapturesOnGrid()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* media = host->document()->addText({}, QStringLiteral("Timeline UI"));
        auto* other=host->document()->addText({},"Magnetic target");
        auto otherTrack = other->timelineTrack();
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
        auto* timeLabel = timelineItems(view.rootObject(), QStringLiteral("timelineCurrentTime")).value(0);
        QVERIFY(timeLabel);
        QCOMPARE(timeLabel->property("text").toString(), QStringLiteral("00:01.333"));
        QVERIFY(!view.rootObject()->property("textInputFocused").toBool());
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
        QCOMPARE(timeLabel->property("text").toString(), QStringLiteral("00:01.333"));
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

#include "backend/domain/media/MediaRuntimeHooks.h"

#include <QPointer>
#include <QTest>

class MediaRuntimeContextTest final : public QObject {
    Q_OBJECT

private slots:
    void callbacksAreIsolatedByCanvas() {
        MediaRuntimeHooks::Context first;
        MediaRuntimeHooks::Context second;
        int firstTicks = 0;
        int secondTicks = 0;
        first.mediaOpacityAnimationTickNotifier = [&firstTicks]() { ++firstTicks; };
        second.mediaOpacityAnimationTickNotifier = [&secondTicks]() { ++secondTicks; };

        first.mediaOpacityAnimationTickNotifier();
        QCOMPARE(firstTicks, 1);
        QCOMPARE(secondTicks, 0);
        second.mediaOpacityAnimationTickNotifier();
        QCOMPARE(firstTicks, 1);
        QCOMPARE(secondTicks, 1);
    }

    void contextLifetimeIsQObjectBound() {
        auto* context = new MediaRuntimeHooks::Context;
        QPointer<MediaRuntimeHooks::Context> guard(context);
        delete context;
        QVERIFY(guard.isNull());
    }
};

QTEST_GUILESS_MAIN(MediaRuntimeContextTest)
#include "tst_MediaRuntimeContext.moc"

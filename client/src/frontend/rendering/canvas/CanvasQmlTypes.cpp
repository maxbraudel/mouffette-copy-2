#include "frontend/rendering/canvas/CanvasQmlTypes.h"

#include "frontend/rendering/canvas/TextEditHelper.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"
#include "frontend/rendering/canvas/TimelineThumbnailItem.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "backend/config/AppConfig.h"
#include "backend/media/MediaResidencyManager.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QtQml/qqml.h>

#include <mutex>

void registerCanvasQmlTypes() {
    static std::once_flag once;
    std::call_once(once, []() {
        qmlRegisterSingletonType<MediaResidencyManager>(
            "Mouffette.Canvas", 1, 0, "MediaMemory",
            [](QQmlEngine*, QJSEngine*) -> QObject* {
                auto* manager = &MediaResidencyManager::instance();
                QQmlEngine::setObjectOwnership(manager, QQmlEngine::CppOwnership);
                return manager;
            });
        qmlRegisterType<TextOutlineItem>("Mouffette.Canvas", 1, 0, "TextOutlineItem");
        qmlRegisterType<RemoteVideoFrameItem>("Mouffette.Canvas", 1, 0, "RemoteVideoFrameItem");
        qmlRegisterType<TimelineThumbnailItem>("Mouffette.Canvas", 1, 0, "TimelineThumbnailItem");
        qmlRegisterSingletonType<TextEditHelper>(
            "Mouffette.Canvas", 1, 0, "TextEditHelper",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new TextEditHelper(); });
        qmlRegisterSingletonType<QQmlPropertyMap>(
            "Mouffette.Canvas", 1, 0, "UiTiming",
            [](QQmlEngine*, QJSEngine*) -> QObject* {
                const AppConfig& config = AppConfig::instance();
                auto* timing = QQmlPropertyMap::create();
                timing->insert(QStringLiteral("contentFadeDurationMs"),
                               config.uiContentFadeDurationMs());
                timing->insert(QStringLiteral("spinnerRotationDurationMs"),
                               config.uiSpinnerRotationDurationMs());
                timing->insert(QStringLiteral("scrollbarHideDelayMs"),
                               config.uiScrollbarHideDelayMs());
                timing->insert(QStringLiteral("inputWatchdogIntervalMs"),
                               config.uiInputWatchdogIntervalMs());
                timing->insert(QStringLiteral("snapFreezeCleanupDelayMs"),
                               config.uiSnapFreezeCleanupDelayMs());
                timing->insert(QStringLiteral("toastAnimationDurationMs"),
                               config.toastAnimationDurationMs());
                timing->freeze();
                return timing;
            });
    });
}

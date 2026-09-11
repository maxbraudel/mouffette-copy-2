#include "frontend/rendering/canvas/CanvasQmlTypes.h"

#include "frontend/rendering/canvas/TextEditHelper.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QtQml/qqml.h>

#include <mutex>

void registerCanvasQmlTypes() {
    static std::once_flag once;
    std::call_once(once, []() {
        qmlRegisterType<TextOutlineItem>("Mouffette.Canvas", 1, 0, "TextOutlineItem");
        qmlRegisterType<RemoteVideoFrameItem>("Mouffette.Canvas", 1, 0, "RemoteVideoFrameItem");
        qmlRegisterSingletonType<TextEditHelper>(
            "Mouffette.Canvas", 1, 0, "TextEditHelper",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new TextEditHelper(); });
    });
}

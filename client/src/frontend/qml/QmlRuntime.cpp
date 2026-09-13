#include "frontend/qml/QmlRuntime.h"

#include <QCoreApplication>
#include <QQmlEngine>

QPointer<QQmlEngine> QmlRuntime::s_engine;

void QmlRuntime::setEngine(QQmlEngine* engine)
{
    s_engine = engine;
}

QQmlEngine* QmlRuntime::engine()
{
    if (!s_engine) {
        // Standalone renderer tests do not create the application composition
        // root. Keep their engine process-owned so deferred QML destruction
        // remains safe after an individual controller has gone away.
        s_engine = new QQmlEngine(QCoreApplication::instance());
    }
    return s_engine;
}

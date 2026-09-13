#pragma once

#include <QPointer>

class QQmlEngine;

// Process-wide access to the single application QML engine. Presentation
// services which create additional QQuickWindows use this registry instead of
// constructing private engines or embedding Qt Quick in widgets.
class QmlRuntime final
{
public:
    static void setEngine(QQmlEngine* engine);
    static QQmlEngine* engine();

private:
    static QPointer<QQmlEngine> s_engine;
};

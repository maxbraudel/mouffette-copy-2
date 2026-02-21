#include "frontend/rendering/canvas/QuickCanvasController.h"

#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWidget>

QuickCanvasController::QuickCanvasController(QObject* parent)
    : QObject(parent)
{
}

bool QuickCanvasController::initialize(QWidget* parentWidget, QString* errorMessage) {
    if (m_quickWidget) {
        return true;
    }

    m_quickWidget = new QQuickWidget(parentWidget);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_quickWidget->setSource(QUrl(QStringLiteral("qrc:/qml/CanvasRoot.qml")));

    if (m_quickWidget->status() == QQuickWidget::Error) {
        if (errorMessage) {
            QStringList messages;
            const auto qmlErrors = m_quickWidget->errors();
            for (const auto& err : qmlErrors) {
                messages.append(err.toString());
            }
            *errorMessage = messages.join(QStringLiteral(" | "));
        }
        delete m_quickWidget;
        m_quickWidget = nullptr;
        return false;
    }

    setScreenCount(0);
    setShellActive(false);
    return true;
}

QWidget* QuickCanvasController::widget() const {
    return m_quickWidget;
}

void QuickCanvasController::setScreenCount(int screenCount) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("screenCount", screenCount);
}

void QuickCanvasController::setShellActive(bool active) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("remoteActive", active);
}

void QuickCanvasController::resetView() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("viewScale", 1.0);
    m_quickWidget->rootObject()->setProperty("panX", 0.0);
    m_quickWidget->rootObject()->setProperty("panY", 0.0);
}

void QuickCanvasController::recenterView() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    QMetaObject::invokeMethod(m_quickWidget->rootObject(), "recenterView");
}

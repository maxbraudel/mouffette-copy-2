#include "frontend/rendering/canvas/QuickCanvasViewAdapter.h"

#include <QQuickItem>
#include <QQuickWidget>

QuickCanvasViewAdapter::QuickCanvasViewAdapter(QQuickWidget* quickWidget, QObject* parent)
    : QObject(parent)
    , m_quickWidget(quickWidget)
{
}

void QuickCanvasViewAdapter::setMediaModel(const QVariantList& model) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("mediaModel", model);
}

void QuickCanvasViewAdapter::setSelectionChromeModel(const QVariantList& model) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("selectionChromeModel", model);
}

void QuickCanvasViewAdapter::setSnapGuidesModel(const QVariantList& model) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("snapGuidesModel", model);
}

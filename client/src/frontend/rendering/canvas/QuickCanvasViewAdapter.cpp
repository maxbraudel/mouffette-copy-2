#include "frontend/rendering/canvas/QuickCanvasViewAdapter.h"
#include "frontend/rendering/canvas/MediaListModel.h"

#include <QQuickItem>
#include <QQuickWidget>

QuickCanvasViewAdapter::QuickCanvasViewAdapter(QQuickWidget* quickWidget, QObject* parent)
    : QObject(parent)
    , m_quickWidget(quickWidget)
{
}

void QuickCanvasViewAdapter::initMediaListModel(MediaListModel* model) {
    m_mediaListModel = model;
    if (!m_quickWidget || !m_quickWidget->rootObject() || !model)
        return;
    // Set the stable C++ model pointer on the QML root once.
    // The Repeater holds this pointer forever; only its row data changes.
    m_quickWidget->rootObject()->setProperty(
        "mediaListModel",
        QVariant::fromValue<QObject*>(model));
}

void QuickCanvasViewAdapter::setMediaModel(const QVariantList& model) {
    if (!m_quickWidget || !m_quickWidget->rootObject())
        return;
    // Update the legacy JS-array used by utility code (hit-testing,
    // selection chrome, input layer, etc.).
    m_quickWidget->rootObject()->setProperty("mediaModel", model);
    // Also push new data into the stable C++ list model so the Repeater
    // receives only fine-grained dataChanged/insertRows/removeRows signals
    // instead of a full delegate teardown.
    if (m_mediaListModel)
        m_mediaListModel->updateFromList(model);
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

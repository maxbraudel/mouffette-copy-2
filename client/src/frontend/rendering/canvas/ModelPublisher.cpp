#include "frontend/rendering/canvas/ModelPublisher.h"

#include "frontend/rendering/canvas/QuickCanvasViewAdapter.h"

void ModelPublisher::publishMediaModel(QuickCanvasViewAdapter* adapter, const QVariantList& mediaModel) const {
    if (!adapter) {
        return;
    }
    adapter->setMediaModel(mediaModel);
}

void ModelPublisher::publishSelectionAndSnapModels(QuickCanvasViewAdapter* adapter,
                                                   const QVariantList& selectionChromeModel,
                                                   const QVariantList& snapGuidesModel) const {
    if (!adapter) {
        return;
    }
    adapter->setSelectionChromeModel(selectionChromeModel);
    adapter->setSnapGuidesModel(snapGuidesModel);
}

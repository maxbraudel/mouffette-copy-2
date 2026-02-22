#ifndef MODELPUBLISHER_H
#define MODELPUBLISHER_H

#include <QVariantList>

class QuickCanvasViewAdapter;

class ModelPublisher {
public:
    void publishMediaModel(QuickCanvasViewAdapter* adapter, const QVariantList& mediaModel) const;
    void publishSelectionAndSnapModels(QuickCanvasViewAdapter* adapter,
                                       const QVariantList& selectionChromeModel,
                                       const QVariantList& snapGuidesModel) const;
};

#endif // MODELPUBLISHER_H

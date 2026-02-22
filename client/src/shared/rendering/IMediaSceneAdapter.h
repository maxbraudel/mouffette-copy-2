#ifndef IMEDIASCENEADAPTER_H
#define IMEDIASCENEADAPTER_H

#include <QList>

class QGraphicsScene;
class ResizableMediaBase;

class IMediaSceneAdapter {
public:
    virtual ~IMediaSceneAdapter() = default;

    virtual QGraphicsScene* scene() const = 0;
    virtual QList<ResizableMediaBase*> enumerateMediaItems() const = 0;
    virtual void refreshInfoOverlay() = 0;
};

#endif // IMEDIASCENEADAPTER_H

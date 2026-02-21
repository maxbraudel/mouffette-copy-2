#ifndef IMEDIASCENEADAPTER_H
#define IMEDIASCENEADAPTER_H

class QGraphicsScene;

class IMediaSceneAdapter {
public:
    virtual ~IMediaSceneAdapter() = default;

    virtual QGraphicsScene* scene() const = 0;
    virtual void refreshInfoOverlay() = 0;
};

#endif // IMEDIASCENEADAPTER_H

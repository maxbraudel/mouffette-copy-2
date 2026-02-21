#ifndef QUICKCANVASCONTROLLER_H
#define QUICKCANVASCONTROLLER_H

#include <QObject>
#include <QHash>
#include <QRectF>

#include "backend/domain/models/ClientInfo.h"

class QQuickWidget;
class QWidget;

class QuickCanvasController : public QObject {
    Q_OBJECT

public:
    explicit QuickCanvasController(QObject* parent = nullptr);
    ~QuickCanvasController() override = default;

    bool initialize(QWidget* parentWidget, QString* errorMessage = nullptr);

    QWidget* widget() const;
    void setScreenCount(int screenCount);
    void setShellActive(bool active);
    void setScreens(const QList<ScreenInfo>& screens);
    void updateRemoteCursor(int globalX, int globalY);
    void hideRemoteCursor();
    void resetView();
    void recenterView();

private:
    void pushStaticLayerModels();
    void pushRemoteCursorState();
    QPointF mapRemoteCursorToQuickScene(int globalX, int globalY, bool* ok) const;
    void rebuildScreenRects();

    QQuickWidget* m_quickWidget = nullptr;
    QList<ScreenInfo> m_screens;
    QHash<int, QRectF> m_sceneScreenRects;
    bool m_remoteCursorVisible = false;
    qreal m_remoteCursorX = 0.0;
    qreal m_remoteCursorY = 0.0;
};

#endif // QUICKCANVASCONTROLLER_H

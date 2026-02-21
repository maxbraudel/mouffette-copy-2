#ifndef QUICKCANVASCONTROLLER_H
#define QUICKCANVASCONTROLLER_H

#include <QObject>

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
    void resetView();
    void recenterView();

private:
    QQuickWidget* m_quickWidget = nullptr;
};

#endif // QUICKCANVASCONTROLLER_H

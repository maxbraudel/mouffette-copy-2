#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWindow>

// Owns presentation policy, independently of the application/session lifecycle.
class WindowPresentation : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QWindow* window READ window WRITE setWindow NOTIFY windowChanged)

public:
    explicit WindowPresentation(QObject* parent = nullptr);
    QWindow* window() const { return m_window; }
    void setWindow(QWindow* window);
    Q_INVOKABLE void open();

    static QRect openingGeometry(const QRect& available, const QMargins& frameMargins);

signals:
    void windowChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void enforcePriority();
    void updateEnforcement();
    void fitToScreen(QScreen* screen);
    QPointer<QWindow> m_window;
    QTimer m_priorityTimer;
};

#pragma once

#include <QObject>
#include <QPointer>
#include <QWindow>

// Owns presentation policy, independently of the application/session lifecycle.
class WindowPresentation : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QWindow* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool alwaysOnTop READ alwaysOnTop WRITE setAlwaysOnTop NOTIFY alwaysOnTopChanged)

public:
    explicit WindowPresentation(QObject* parent = nullptr);
    ~WindowPresentation() override;
    QWindow* window() const { return m_window; }
    void setWindow(QWindow* window);
    bool alwaysOnTop() const { return m_alwaysOnTop; }
    void setAlwaysOnTop(bool enabled);
    Q_INVOKABLE void open();
    Q_INVOKABLE void toggle();

    static QRect openingGeometry(const QRect& available, const QMargins& frameMargins);

signals:
    void windowChanged();
    void alwaysOnTopChanged();

private:
    void fitToScreen(QScreen* screen);
    QPointer<QWindow> m_window;
    bool m_alwaysOnTop = true;
};

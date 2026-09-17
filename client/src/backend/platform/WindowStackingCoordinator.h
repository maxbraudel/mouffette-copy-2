#pragma once

#include <QAbstractNativeEventFilter>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWindow>

// One ordering policy for the interactive shell and passive scene surfaces.
// Registration never shows a window. Only the scene owner can activate it.
class WindowStackingCoordinator final : public QObject, public QAbstractNativeEventFilter
{
public:
    static WindowStackingCoordinator& instance();
    void registerControlWindow(QWindow* window);
    void registerSceneWindow(QWindow* window);
    void setSceneWindowActive(QWindow* window, bool active);
    void unregisterWindow(QWindow* window);
    void enforce();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

private:
    explicit WindowStackingCoordinator(QObject* parent);
    ~WindowStackingCoordinator() override;
    void registerWindow(QWindow* window, bool scene);
    void scheduleEnforcement();
    struct Entry {
        QPointer<QWindow> window;
        bool scene = false;
        bool active = false;
        QMetaObject::Connection destroyed;
    };
    QList<Entry> m_windows;
    QTimer m_timer;
    QTimer m_deferred;
    bool m_enforcing = false;
#ifdef Q_OS_WIN
    void* m_windowEventHook = nullptr;
    void* m_foregroundEventHook = nullptr;
#endif
};

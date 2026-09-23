#pragma once

#include <QObject>
#include <QString>

class QWindow;

// Every window owned by this process stays out of desktop capture, including
// received scenes. Native Windows affinity applies before a surface is shown;
// macOS uses ScreenCaptureKit's application exclusion directly.
class WindowCaptureExclusion final : public QObject
{
    Q_OBJECT
public:
    static WindowCaptureExclusion& instance();
    // Windows capture must call this before starting, and stop on an unsafe
    // transition. A failed affinity is never silently treated as protection.
    bool prepareForCapture(QString* error = nullptr);
    bool captureAllowed() const { return m_captureError.isEmpty(); }
    QString captureError() const { return m_captureError; }

signals:
    void captureSafetyChanged(bool allowed, const QString& reason);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit WindowCaptureExclusion(QObject* parent);
    ~WindowCaptureExclusion() override;
    void failCapture(const QString& reason);
    void applyWindow(QWindow* window);
#ifdef Q_OS_WIN
    bool installNativeWindowHook();
    bool applyNativeWindow(quintptr handle);
    void* m_nativeWindowHook = nullptr;
#endif
    QString m_captureError;
    bool m_applying = false;
};

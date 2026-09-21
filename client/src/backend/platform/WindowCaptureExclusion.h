#pragma once

#include <QAbstractNativeEventFilter>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QWindow;

// Control windows stay local; only explicitly registered scene surfaces may
// appear in a desktop capture. The native capture applies this policy before
// encoding, so excluding the shell does not add a per-frame pixel operation.
class WindowCaptureExclusion final : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    static WindowCaptureExclusion& instance();
    void setSceneWindow(QWindow* window, bool scene);
    QList<QWindow*> sceneWindows() const;

    // Windows capture must call this before starting, and stop on an unsafe
    // transition. A failed affinity is never silently treated as protection.
    bool prepareForCapture(QString* error = nullptr);
    bool captureAllowed() const { return m_captureError.isEmpty(); }
    QString captureError() const { return m_captureError; }

signals:
    void sceneWindowsChanged();
    void captureSafetyChanged(bool allowed, const QString& reason);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

private:
    explicit WindowCaptureExclusion(QObject* parent);
    ~WindowCaptureExclusion() override;
    void failCapture(const QString& reason);
    void applyWindow(QWindow* window);
#ifdef Q_OS_WIN
    bool applyNativeWindow(quintptr handle);
#endif
    QList<QPointer<QWindow>> m_sceneWindows;
    QString m_captureError;
    bool m_applying = false;
};

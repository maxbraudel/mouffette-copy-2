#ifndef SCREENNAVIGATIONMANAGER_H
#define SCREENNAVIGATIONMANAGER_H

#include <QObject>
#include <QPointer>
#include <QString>

class QStackedWidget; class QWidget; class QPushButton; class QTimer; class QGraphicsOpacityEffect; class QPropertyAnimation; class ScreenCanvas; class SpinnerWidget; class ClientInfo;

// Manages navigation between client list and screen view and associated loading/animation UX.
class ScreenNavigationManager : public QObject {
    Q_OBJECT
public:
    explicit ScreenNavigationManager(QObject* parent = nullptr);

    struct Widgets {
        QStackedWidget* stack = nullptr;
        QWidget* clientListPage = nullptr;
        QWidget* screenViewPage = nullptr;
        QPushButton* backButton = nullptr;
        QStackedWidget* canvasStack = nullptr; // index 0: spinner, 1: canvas
        SpinnerWidget* loadingSpinner = nullptr;
        QGraphicsOpacityEffect* spinnerOpacity = nullptr;
        QPropertyAnimation* spinnerFade = nullptr;
        QGraphicsOpacityEffect* canvasOpacity = nullptr;
        QPropertyAnimation* canvasFade = nullptr;
        QGraphicsOpacityEffect* volumeOpacity = nullptr;
        QPropertyAnimation* volumeFade = nullptr;
        ScreenCanvas* screenCanvas = nullptr;
        SpinnerWidget* inlineSpinner = nullptr; // Small spinner for reconnection
        bool* canvasContentEverLoaded = nullptr; // Pointer to flag tracking if content was loaded
    };

    void setWidgets(const Widgets& w); // must be called before usage
    void setDurations(int loaderDelayMs, int loaderFadeMs, int canvasFadeMs);

    void showScreenView(const ClientInfo& client); // triggers requestScreens + watch
    void showClientList();
    // Called when screens data has arrived and we can display the canvas
    void revealCanvas();
    // Immediately switch the canvas area to a loading state (spinner visible,
    // canvas and volume overlays hidden). Intended for connection loss while
    // on the screen view so the UI reflects the disconnected state.
    void enterLoadingStateImmediate();

    bool isOnScreenView() const;
    QString currentClientId() const { return m_currentClientId; }

signals:
    void requestScreens(const QString& clientId);
    void watchTargetRequested(const QString& clientId);
    void screenViewEntered(const QString& clientId);
    void clientListEntered();

private slots:
    void onLoaderDelayTimeout();

private:
    void ensureLoaderTimer();
    void startSpinnerDelayed();
    void stopSpinner();
    void fadeInCanvas();

    Widgets m_w;
    QString m_currentClientId;
    QTimer* m_loaderDelayTimer = nullptr;
    int m_loaderDelayMs = 1000;
    int m_loaderFadeDurationMs = 500;
    int m_canvasFadeDurationMs = 50;
};

#endif // SCREENNAVIGATIONMANAGER_H

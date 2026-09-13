#ifndef SCREENNAVIGATIONMANAGER_H
#define SCREENNAVIGATIONMANAGER_H

#include <QObject>
#include <QPointer>
#include <QString>

class ClientInfo;
class ICanvasHost;

// Non-visual navigation state shared by the runtime and Qt Quick shell.
// Presentation and transitions live in QML; this object only records the
// current destination and whether the active canvas is waiting for data.
class ScreenNavigationManager final : public QObject
{
    Q_OBJECT

public:
    explicit ScreenNavigationManager(QObject* parent = nullptr);

    void setActiveCanvas(ICanvasHost* canvas);
    void showScreenView(const ClientInfo& client, bool hasCachedContent = false);
    void refreshActiveClientPreservingCanvas(const ClientInfo& client);
    void showClientList();
    void revealCanvas();
    void enterLoadingStateImmediate();

    bool isOnScreenView() const { return m_onScreenView; }
    bool isLoading() const { return m_loading; }
    bool canvasVisible() const { return m_canvasVisible; }
    QString currentClientId() const { return m_currentClientId; }

signals:
    void screenViewEntered(const QString& clientId);
    void clientListEntered();
    void presentationChanged();

private:
    void setPresentation(bool loading, bool canvasVisible);

    QPointer<ICanvasHost> m_activeCanvas;
    QString m_currentClientId;
    bool m_onScreenView = false;
    bool m_loading = false;
    bool m_canvasVisible = false;
};

#endif // SCREENNAVIGATIONMANAGER_H

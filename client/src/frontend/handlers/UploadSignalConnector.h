#ifndef UPLOADSIGNALCONNECTOR_H
#define UPLOADSIGNALCONNECTOR_H

#include <QObject>

class ApplicationRuntime;
class UploadManager;
class WebSocketClient;

/**
 * @brief Connects all upload-related signals from UploadManager and WebSocketClient to ApplicationRuntime handlers
 * 
 * Connects upload-domain signals to workspace state and notifications.
 * This class encapsulates all upload signal wiring logic (~140 lines of connections)
 */
class UploadSignalConnector : public QObject
{
    Q_OBJECT

public:
    explicit UploadSignalConnector(QObject* parent = nullptr);

    /**
     * @brief Connect all upload signals to ApplicationRuntime handlers
     * @param mainWindow The ApplicationRuntime instance to connect signals to
     * @param uploadManager The UploadManager to connect signals from
     * @param webSocketClient The WebSocketClient to connect signals from
     * @param uploadSignalsConnected Reference to flag tracking connection state
     * 
     * This method is idempotent - it will only connect signals once
     */
    void connectAllSignals(
        ApplicationRuntime* mainWindow,
        UploadManager* uploadManager,
        WebSocketClient* webSocketClient,
        bool& uploadSignalsConnected
    );
};

#endif // UPLOADSIGNALCONNECTOR_H

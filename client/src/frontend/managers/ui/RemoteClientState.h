#pragma once

#include <QString>
#include <QMetaType>
#include "backend/domain/models/ClientInfo.h"

/**
 * @brief Represents the complete state of the remote client info UI
 * 
 * This is a value object that encapsulates all the information needed
 * to render the remote client info container. Using a state object
 * allows atomic updates and eliminates flickering.
 * 
 * Usage:
 *   RemoteClientState state = RemoteClientState::connected(clientInfo, 75);
 *   mainWindow->setRemoteClientState(state);
 */
struct RemoteClientState {
    enum ConnectionStatus {
        Disconnected,
        Connecting,
        Reconnecting,
        Connected,
        Error
    };

    // Core state
    ClientInfo clientInfo;
    ConnectionStatus connectionStatus = Disconnected;
    
    // Volume state
    bool volumeVisible = false;
    int volumePercent = -1;
    
    // UI visibility
    bool statusVisible = true;
    bool spinnerActive = false;
    
    // Factory methods for common states
    static RemoteClientState disconnected() {
        RemoteClientState state;
        state.connectionStatus = Disconnected;
        state.volumeVisible = false;
        state.statusVisible = true;
        state.spinnerActive = false;
        return state;
    }
    
    static RemoteClientState connecting(const ClientInfo& client) {
        RemoteClientState state;
        state.clientInfo = client;
        state.connectionStatus = Connecting;
        state.volumeVisible = false;
        state.statusVisible = true;
        state.spinnerActive = true;
        return state;
    }
    
    static RemoteClientState connected(const ClientInfo& client, int volume = -1) {
        RemoteClientState state;
        state.clientInfo = client;
        state.connectionStatus = Connected;
        state.volumeVisible = (volume >= 0);
        state.volumePercent = volume;
        state.statusVisible = true;
        state.spinnerActive = false;
        return state;
    }
    
    static RemoteClientState error() {
        RemoteClientState state;
        state.connectionStatus = Error;
        state.volumeVisible = false;
        state.statusVisible = true;
        state.spinnerActive = false;
        return state;
    }
    
    // Utility methods
    QString statusText() const {
        switch (connectionStatus) {
            case Disconnected: return "DISCONNECTED";
            case Connecting: return "CONNECTING...";
            case Reconnecting: return "RECONNECTING...";
            case Connected: return "CONNECTED";
            case Error: return "ERROR";
        }
        return "UNKNOWN";
    }
    
    bool isStableState() const {
        return connectionStatus == Connected || 
               connectionStatus == Disconnected || 
               connectionStatus == Error;
    }
    
    bool shouldShowVolume() const {
        return volumeVisible && volumePercent >= 0 && connectionStatus == Connected;
    }
};

Q_DECLARE_METATYPE(RemoteClientState)

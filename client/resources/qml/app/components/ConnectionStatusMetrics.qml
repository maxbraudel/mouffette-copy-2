import QtQuick

StateTextMetrics {
    property bool uppercase: false
    property bool sceneStatus: false
    textVariants: sceneStatus ? ["Live", "Degraded"] : uppercase
        ? ["AVAILABLE", "CONNECTED", "DISCONNECTED", "CONNECTING",
           "RECONNECTING", "DISCONNECTING", "UNREACHABLE", "DEGRADED", "CONNECTION ERROR"]
        : ["Available", "Connected", "Disconnected", "Connecting",
           "Reconnecting", "Disconnecting", "Unreachable", "Degraded", "Connection error"]
}

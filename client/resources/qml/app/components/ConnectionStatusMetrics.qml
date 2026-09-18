import QtQuick

StateTextMetrics {
    property bool uppercase: false
    property bool sceneStatus: false
    textVariants: sceneStatus ? ["Live", "Degraded"] : uppercase
        ? ["AVAILABLE", "CONNECTED", "DISCONNECTED", "CONNECTING",
           "DISCONNECTING", "UNREACHABLE", "DEGRADED", "CONNECTION ERROR"]
        : ["Available", "Connected", "Disconnected", "Connecting",
           "Disconnecting", "Unreachable", "Degraded", "Connection error"]
}

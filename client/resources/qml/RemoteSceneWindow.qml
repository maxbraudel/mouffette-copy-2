import QtQuick
import QtQuick.Window

Window {
    id: window
    objectName: "remoteSceneWindow"
    color: "transparent"
    visible: false

    property var mediaListModel: null
    signal spanReady(string mediaId, string spanId)

    RemoteSceneRoot {
        id: sceneRoot
        objectName: "remoteSceneRoot"
        anchors.fill: parent
        mediaListModel: window.mediaListModel

        onSpanReady: (mediaId, spanId) => window.spanReady(mediaId, spanId)
    }
}

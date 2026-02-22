import QtQuick 2.15

Item {
    id: mediaLayer
    property var mediaModel: []
    property Item contentItem: null
    property var sourceUrlResolver: function(path) { return path }
    default property alias layerChildren: layerRoot.data

    signal mediaSelectRequested(string mediaId, bool additive)
    signal mediaMoveEnded(string mediaId, real sceneX, real sceneY)
    signal textCommitRequested(string mediaId, string text)

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}

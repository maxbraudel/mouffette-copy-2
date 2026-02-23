import QtQuick 2.15

Item {
    id: selectionLayer

    property Item contentItem: null
    property Item viewportItem: null
    property var mediaModel: []
    property var selectionModel: []
    property var snapGuidesModel: []
    property var interactionController: null
    property var inputCoordinator: null
    property string draggedMediaId: ""
    property real dragOffsetViewX: 0.0
    property real dragOffsetViewY: 0.0
    default property alias layerChildren: layerRoot.data

    signal mediaResizeRequested(string mediaId, string handleId, real sceneX, real sceneY, bool snap)
    signal mediaResizeEnded(string mediaId)

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}

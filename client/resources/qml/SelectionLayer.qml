import QtQuick 2.15

Item {
    id: selectionLayer

    property Item contentItem: null
    property Item viewportItem: null
    property var mediaModel: []
    property var selectionModel: []
    property var snapGuidesModel: []
    property var interactionController: null
    property string draggedMediaId: ""
    property real dragOffsetViewX: 0.0
    property real dragOffsetViewY: 0.0

    signal mediaResizeRequested(string mediaId, string handleId, real sceneX, real sceneY, bool snap)
    signal mediaResizeEnded(string mediaId)

    SnapGuides {
        id: snapGuides
        anchors.fill: parent
        contentItem: selectionLayer.contentItem
        viewportItem: selectionLayer.viewportItem
        guidesModel: selectionLayer.snapGuidesModel
        z: 89000
    }

    SelectionChrome {
        id: selectionChrome
        anchors.fill: parent
        contentItem: selectionLayer.contentItem
        viewportItem: selectionLayer.viewportItem
        interactionController: selectionLayer.interactionController
        mediaModel: selectionLayer.mediaModel
        selectionModel: selectionLayer.selectionModel
        draggedMediaId: selectionLayer.draggedMediaId
        dragOffsetViewX: selectionLayer.dragOffsetViewX
        dragOffsetViewY: selectionLayer.dragOffsetViewY
        z: 90000
        onResizeRequested: function(mediaId, handleId, sceneX, sceneY, snap) {
            selectionLayer.mediaResizeRequested(mediaId, handleId, sceneX, sceneY, snap)
        }
        onResizeEnded: function(mediaId) {
            selectionLayer.mediaResizeEnded(mediaId)
        }
    }
}

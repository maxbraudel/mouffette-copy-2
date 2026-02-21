import QtQuick 2.15

// Renders the selection border and resize handles as a viewport-space overlay.
// Move drag is handled natively by each media item's DragHandler in contentRoot.
// This component only handles resize (8 handles) and provides visual chrome.
Item {
    id: root

    property var selectionModel: []
    property Item contentItem: null
    property Item viewportItem: null
    property int handleSize: 10
    property bool interacting: false

    // Live drag offset injected from CanvasRoot — chrome follows content without model repush
    property string draggedMediaId: ""
    property real dragOffsetViewX: 0.0
    property real dragOffsetViewY: 0.0

    signal resizeRequested(string mediaId, string handleId, real sceneX, real sceneY, bool snap)
    signal resizeEnded(string mediaId)

    Repeater {
        model: root.selectionModel

        delegate: Item {
            id: chrome
            property var entry: modelData
            readonly property real _viewScale: root.contentItem ? root.contentItem.scale : 1.0
            readonly property real _contentX: root.contentItem ? root.contentItem.x : 0.0
            readonly property real _contentY: root.contentItem ? root.contentItem.y : 0.0
            readonly property bool beingDragged: !!entry && root.draggedMediaId !== "" && root.draggedMediaId === entry.mediaId

            readonly property point p1: {
                var _vs = _viewScale
                var _cx = _contentX
                var _cy = _contentY
                if (!root.contentItem || !root.viewportItem || !entry)
                    return Qt.point(0, 0)
                return root.contentItem.mapToItem(root.viewportItem, entry.x, entry.y)
            }

            readonly property point p2: {
                var _vs = _viewScale
                var _cx = _contentX
                var _cy = _contentY
                if (!root.contentItem || !root.viewportItem || !entry)
                    return Qt.point(1, 1)
                return root.contentItem.mapToItem(
                            root.viewportItem,
                            entry.x + Math.max(1, entry.width),
                            entry.y + Math.max(1, entry.height))
            }

            enabled: !!entry
            visible: !!entry
            z: 0

            x: Math.round(Math.min(p1.x, p2.x)) + (beingDragged ? root.dragOffsetViewX : 0)
            y: Math.round(Math.min(p1.y, p2.y)) + (beingDragged ? root.dragOffsetViewY : 0)
            width: Math.max(1, Math.round(Math.abs(p2.x - p1.x)))
            height: Math.max(1, Math.round(Math.abs(p2.y - p1.y)))

            Canvas {
                anchors.fill: parent
                antialiasing: false
                enabled: false

                Component.onCompleted: requestPaint()

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()

                    var w = width
                    var h = height
                    if (w <= 1 || h <= 1)
                        return

                    ctx.setLineDash([4, 4])
                    ctx.lineWidth = 1

                    ctx.strokeStyle = "#FFFFFF"
                    ctx.lineDashOffset = 0
                    ctx.strokeRect(0.5, 0.5, w - 1, h - 1)

                    ctx.strokeStyle = "#4A90E2"
                    ctx.lineDashOffset = 4
                    ctx.strokeRect(0.5, 0.5, w - 1, h - 1)
                }
            }

            Repeater {
                model: [
                    { x: 0, y: 0, handleId: "top-left" },
                    { x: width * 0.5, y: 0, handleId: "top-mid" },
                    { x: width, y: 0, handleId: "top-right" },
                    { x: 0, y: height * 0.5, handleId: "left-mid" },
                    { x: width, y: height * 0.5, handleId: "right-mid" },
                    { x: 0, y: height, handleId: "bottom-left" },
                    { x: width * 0.5, y: height, handleId: "bottom-mid" },
                    { x: width, y: height, handleId: "bottom-right" }
                ]

                delegate: Rectangle {
                    width: root.handleSize
                    height: root.handleSize
                    radius: 1
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: "#4A90E2"
                    antialiasing: false
                    enabled: true

                    x: Math.round(modelData.x - width * 0.5)
                    y: Math.round(modelData.y - height * 0.5)

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        hoverEnabled: false
                        scrollGestureEnabled: false
                        onPressed: function(mouse) {
                            mouse.accepted = true
                        }
                    }

                    DragHandler {
                        target: null
                        acceptedButtons: Qt.LeftButton
                        onActiveChanged: {
                            root.interacting = active
                            if (!active) {
                                root.resizeEnded(entry ? (entry.mediaId || "") : "")
                            }
                        }
                        onTranslationChanged: {
                            if (!entry || !root.contentItem || !root.viewportItem)
                                return
                            var centerVX = chrome.x + modelData.x + translation.x
                            var centerVY = chrome.y + modelData.y + translation.y
                            var scenePt = root.contentItem.mapFromItem(root.viewportItem, centerVX, centerVY)
                            var snapEnabled = (Qt.application.keyboardModifiers & Qt.ShiftModifier) !== 0
                            root.resizeRequested(entry.mediaId || "", modelData.handleId || "", scenePt.x, scenePt.y, snapEnabled)
                        }
                    }
                }
            }
        }
    }
}

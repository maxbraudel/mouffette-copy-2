import QtQuick 2.15

Item {
    id: root

    property var guidesModel: []
    property Item contentItem: null
    property Item viewportItem: null

    Repeater {
        model: root.guidesModel

        delegate: Canvas {
            property var entry: modelData
            readonly property real _viewScale: root.contentItem ? root.contentItem.scale : 1.0
            readonly property real _contentX: root.contentItem ? root.contentItem.x : 0.0
            readonly property real _contentY: root.contentItem ? root.contentItem.y : 0.0

            readonly property point p1: {
                var _vs = _viewScale
                var _cx = _contentX
                var _cy = _contentY
                if (!root.contentItem || !root.viewportItem || !entry)
                    return Qt.point(0, 0)
                return root.contentItem.mapToItem(root.viewportItem, entry.x1, entry.y1)
            }

            readonly property point p2: {
                var _vs = _viewScale
                var _cx = _contentX
                var _cy = _contentY
                if (!root.contentItem || !root.viewportItem || !entry)
                    return Qt.point(0, 0)
                return root.contentItem.mapToItem(root.viewportItem, entry.x2, entry.y2)
            }

            anchors.fill: parent
            enabled: false
            visible: !!entry
            antialiasing: false

            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()

                var dx = p2.x - p1.x
                var dy = p2.y - p1.y
                var length = Math.sqrt(dx * dx + dy * dy)
                if (length < 0.5)
                    return

                var startX = p1.x
                var startY = p1.y
                var endX = p2.x
                var endY = p2.y

                if (Math.abs(dx) < Math.abs(dy)) {
                    var snappedX = Math.round(startX) + 0.5
                    startX = snappedX
                    endX = snappedX
                } else {
                    var snappedY = Math.round(startY) + 0.5
                    startY = snappedY
                    endY = snappedY
                }

                ctx.setLineDash([4, 4])
                ctx.lineWidth = 1
                ctx.strokeStyle = "#48C2FF"
                ctx.beginPath()
                ctx.moveTo(startX, startY)
                ctx.lineTo(endX, endY)
                ctx.stroke()
            }
        }
    }
}

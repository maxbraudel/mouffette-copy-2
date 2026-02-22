import QtQuick 2.15

Item {
    id: root

    property var guidesModel: []
    property Item contentItem: null
    property Item viewportItem: null

    Repeater {
        model: root.guidesModel

        delegate: Item {
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
                return Qt.point(_cx + entry.x1 * _vs, _cy + entry.y1 * _vs)
            }

            readonly property point p2: {
                var _vs = _viewScale
                var _cx = _contentX
                var _cy = _contentY
                if (!root.contentItem || !root.viewportItem || !entry)
                    return Qt.point(0, 0)
                return Qt.point(_cx + entry.x2 * _vs, _cy + entry.y2 * _vs)
            }

            readonly property bool vertical: Math.abs(p2.x - p1.x) < Math.abs(p2.y - p1.y)
            readonly property real minX: Math.min(p1.x, p2.x)
            readonly property real minY: Math.min(p1.y, p2.y)
            readonly property real guideLength: vertical ? Math.abs(p2.y - p1.y) : Math.abs(p2.x - p1.x)

            anchors.fill: parent
            enabled: false
            visible: !!entry && guideLength >= 0.5

            Rectangle {
                x: vertical ? Math.round(minX) : Math.round(minX)
                y: vertical ? Math.round(minY) : Math.round(minY)
                width: vertical ? 1 : Math.max(1, Math.round(parent.guideLength))
                height: vertical ? Math.max(1, Math.round(parent.guideLength)) : 1
                color: "#48C2FF"
                opacity: 0.95
                antialiasing: false
            }
        }
    }
}

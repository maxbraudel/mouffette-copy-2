import QtQuick 2.15

Rectangle {
    id: root

    property real zoneX: 0
    property real zoneY: 0
    property real zoneWidth: 0
    property real zoneHeight: 0
    property color fillColor: "#5A808080"

    x: zoneX
    y: zoneY
    width: zoneWidth
    height: zoneHeight
    z: -500

    color: fillColor
    border.width: 0
}

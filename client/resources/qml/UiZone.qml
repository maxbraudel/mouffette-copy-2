import QtQuick
import Mouffette.App
Rectangle {
    id: root

    property real zoneX: 0
    property real zoneY: 0
    property real zoneWidth: 0
    property real zoneHeight: 0
    property color fillColor: Theme.uiZoneFill

    x: zoneX
    y: zoneY
    width: zoneWidth
    height: zoneHeight
    z: -500

    color: fillColor
    border.width: 0
}

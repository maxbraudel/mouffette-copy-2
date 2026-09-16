import QtQuick
import Mouffette.App as AppStyle
Rectangle {
    id: root

    property real zoneX: 0
    property real zoneY: 0
    property real zoneWidth: 0
    property real zoneHeight: 0
    property color fillColor: AppStyle.Theme.uiZoneFill

    x: zoneX
    y: zoneY
    width: zoneWidth
    height: zoneHeight
    z: -500

    color: fillColor
    border.width: 0
}

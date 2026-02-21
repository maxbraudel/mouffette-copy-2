import QtQuick 2.15

Rectangle {
    id: root

    property real screenX: 0
    property real screenY: 0
    property real screenWidth: 0
    property real screenHeight: 0
    property bool primary: false

    x: screenX
    y: screenY
    width: screenWidth
    height: screenHeight
    z: -1000

    color: primary ? "#B44A90E2" : "#B4505050"
    border.color: primary ? "#4A90E2" : "#A0A0A0"
    border.width: 1
}

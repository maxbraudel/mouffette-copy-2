import QtQuick 2.15

Rectangle {
    id: root
    color: "#10131a"

    property int screenCount: 0
    property bool remoteActive: false

    Rectangle {
        anchors.fill: parent
        anchors.margins: 12
        radius: 10
        color: "#151b24"
        border.width: 1
        border.color: remoteActive ? "#4a90e2" : "#2b3444"
    }

    Text {
        anchors.centerIn: parent
        color: "#7f8aa3"
        text: "Quick Canvas Shell · screens: " + screenCount
        font.pixelSize: 14
    }
}

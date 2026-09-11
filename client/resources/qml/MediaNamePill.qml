import QtQuick 2.15

// Reused by the selected-media overlay and by the pre-drop preview so the
// filename does not move or restyle during the handoff.
Rectangle {
    id: root

    property string displayName: ""

    radius: 6
    color: "#F2323232"
    border.color: "#FF646464"
    border.width: 1

    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        text: root.displayName
        color: "#FFFFFF"
        font.pixelSize: 16
        elide: Text.ElideRight
        maximumLineCount: 1
        horizontalAlignment: Text.AlignHCenter
    }
}

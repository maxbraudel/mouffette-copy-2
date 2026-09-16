import QtQuick
import Mouffette.App as AppStyle
// Reused by the selected-media overlay and by the pre-drop preview so the
// filename uses the same presentation for every selected media item.
Rectangle {
    id: root

    property string displayName: ""

    radius: 6
    color: AppStyle.Theme.overlayBackground
    border.color: AppStyle.Theme.overlayBorder
    border.width: 1

    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        text: root.displayName
        color: AppStyle.Theme.overlayText
        font.pixelSize: 16
        elide: Text.ElideRight
        maximumLineCount: 1
        horizontalAlignment: Text.AlignHCenter
    }
}

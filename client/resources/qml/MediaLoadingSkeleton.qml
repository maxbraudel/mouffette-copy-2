import QtQuick
import QtQuick.Controls
import Mouffette.App as AppStyle

Item {
    id: root
    property bool failed: false
    property string errorText: ""
    property bool showErrorIndicator: true
    property color color: AppStyle.Theme.mediaPlaceholder
    property real pulseOpacity: 0.28
    readonly property bool pulsing: visible && !failed

    Rectangle {
        anchors.fill: parent
        color: root.failed ? AppStyle.Theme.errorBackground : root.color
        opacity: root.failed ? 1 : root.pulseOpacity
    }
    SequentialAnimation on pulseOpacity {
        running: root.pulsing
        loops: Animation.Infinite
        NumberAnimation { to: 0.5; duration: 700; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0.28; duration: 700; easing.type: Easing.InOutSine }
    }
    Text {
        objectName: "mediaLoadingErrorIndicator"
        anchors.centerIn: parent
        visible: root.failed && root.showErrorIndicator
        text: "!"
        color: AppStyle.Theme.errorText
        font.bold: true
        font.pixelSize: Math.max(10, Math.min(24, parent.height - 4))
    }
    HoverHandler { id: hover }
    ToolTip.visible: root.failed && hover.hovered && root.errorText.length > 0
    ToolTip.text: root.errorText
}

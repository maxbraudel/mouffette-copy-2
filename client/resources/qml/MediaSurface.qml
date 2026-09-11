import QtQuick 2.15

// Shared loading surface for every raster media representation. Geometry is
// owned by the caller; this component only guarantees that an exact-size grey
// placeholder exists before the decoded pixels become available.
Item {
    id: root

    property bool contentReady: false
    property int fadeDuration: 80
    property color placeholderColor: "#F2323232"
    default property alias content: contentHost.data

    Rectangle {
        anchors.fill: parent
        color: root.placeholderColor
        border.width: 0
    }

    Item {
        id: contentHost
        anchors.fill: parent
        opacity: root.contentReady ? 1.0 : 0.0

        Behavior on opacity {
            NumberAnimation {
                duration: root.fadeDuration
                easing.type: Easing.OutCubic
            }
        }
    }
}

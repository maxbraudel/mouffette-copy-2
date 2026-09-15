import QtQuick
import Mouffette.Canvas
// Shared loading surface for every raster media representation. Geometry is
// owned by the caller; this component only guarantees that an exact-size grey
// placeholder exists before the decoded pixels become available.
Item {
    id: root

    property bool contentReady: false
    // During a drop handoff the old preview fully covers this surface. Reveal
    // the decoded final content atomically behind it, then let the preview fade
    // away. Running both fades together would expose the grey placeholder.
    property bool revealImmediately: false
    property int fadeDuration: UiTiming.contentFadeDurationMs
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
            enabled: !root.revealImmediately
            NumberAnimation {
                duration: root.fadeDuration
                easing.type: Easing.OutCubic
            }
        }
    }
}

import QtQuick
import QtQuick.Window

Item {
    id: root
    property bool contentReady: false
    property bool firstFramePresented: false
    property bool requireInitialSkeleton: true
    readonly property bool revealed: contentReady && (!requireInitialSkeleton || firstFramePresented)
    default property alias content: contentHost.data

    Connections {
        target: root.Window.window
        function onFrameSwapped() { root.firstFramePresented = true }
    }

    Rectangle {
        id: skeleton
        objectName: "mediaLoadingSkeleton"
        anchors.fill: parent
        visible: !root.revealed
        color: "#808080"
        opacity: 0.28
        SequentialAnimation on opacity {
            running: skeleton.visible
            loops: Animation.Infinite
            NumberAnimation { to: 0.5; duration: 700; easing.type: Easing.InOutSine }
            NumberAnimation { to: 0.28; duration: 700; easing.type: Easing.InOutSine }
        }
    }

    Item {
        id: contentHost
        anchors.fill: parent
        visible: root.revealed
    }
}

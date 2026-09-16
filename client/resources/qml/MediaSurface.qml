import QtQuick
import QtQuick.Window
import Mouffette.Canvas

Item {
    id: root
    property bool contentReady: false
    property bool firstFramePresented: false
    property bool requireInitialSkeleton: true
    readonly property bool revealed: contentReady && (!requireInitialSkeleton || firstFramePresented)
    property real revealProgress: 0
    default property alias content: contentHost.data

    states: State {
        name: "revealed"
        when: root.revealed
        PropertyChanges { target: root; revealProgress: 1 }
    }

    // A transition owns the readiness change and its animation together.
    // Returning to the skeleton is immediate; remote scenes own their fades.
    transitions: Transition {
        to: "revealed"
        enabled: root.requireInitialSkeleton
        NumberAnimation {
            property: "revealProgress"
            duration: UiTiming.contentFadeDurationMs
            easing.type: Easing.InOutSine
        }
    }

    Connections {
        target: root.Window.window
        function onFrameSwapped() { root.firstFramePresented = true }
    }

    Rectangle {
        id: skeleton
        objectName: "mediaLoadingSkeleton"
        anchors.fill: parent
        visible: root.revealProgress < 1
        color: "#808080"
        property real pulseOpacity: 0.28
        opacity: pulseOpacity * (1 - root.revealProgress)
        SequentialAnimation on pulseOpacity {
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
        opacity: root.revealProgress
    }
}

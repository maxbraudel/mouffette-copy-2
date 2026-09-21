import QtQuick
import QtQuick.Window
import Mouffette.Canvas

Item {
    id: root
    property bool residencyReady: false
    property bool contentReady: false
    property bool firstFramePresented: false
    property bool requireInitialSkeleton: true
    property string loadingState: ""
    property string loadingError: ""
    property bool initialized: false
    property bool loadingRevealPending: false
    readonly property bool renderingAllowed: initialized && (!loadingRevealPending || firstFramePresented)
    readonly property bool revealed: residencyReady && contentReady && renderingAllowed
    property real revealProgress: 0
    default property alias content: contentHost.data

    // Residency outlives this visual. A page revisit must create its rendering
    // surface immediately, even though that new surface has no frame yet.
    // Only observing missing resident data arms the skeleton gate and fade.
    Component.onCompleted: {
        loadingRevealPending = requireInitialSkeleton && !residencyReady
        initialized = true
    }
    onResidencyReadyChanged: {
        if (initialized && !residencyReady)
            loadingRevealPending = requireInitialSkeleton
    }

    states: State {
        name: "revealed"
        when: root.revealed
        PropertyChanges { target: root; revealProgress: 1 }
    }

    // A transition owns the readiness change and its animation together.
    // Returning to the skeleton is immediate; remote scenes own their fades.
    transitions: Transition {
        to: "revealed"
        enabled: root.loadingRevealPending
        onRunningChanged: {
            if (!running && root.revealed && root.revealProgress === 1)
                root.loadingRevealPending = false
        }
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

    MediaLoadingSkeleton {
        id: skeleton
        objectName: "mediaLoadingSkeleton"
        anchors.fill: parent
        visible: root.revealProgress < 1
        failed: root.loadingState === "error"
        errorText: root.loadingError
        opacity: 1 - root.revealProgress
    }

    Item {
        id: contentHost
        anchors.fill: parent
        visible: root.revealed
        opacity: root.revealProgress
    }
}

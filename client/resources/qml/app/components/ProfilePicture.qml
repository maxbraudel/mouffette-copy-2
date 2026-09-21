pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

Item {
    id: root

    property var controller: null
    property string endpointId: ""
    property url source: ""
    readonly property url defaultSource: "qrc:/icons/default-profile-picture.jpg"
    readonly property url resolvedSource: {
        // Profile bytes stay in the controller's RAM cache. Its revision also
        // invalidates this binding when a peer removes or replaces a picture.
        var revision = controller ? controller.profileRevision : 0
        if (source.toString().length > 0) return source
        return controller && endpointId.length > 0
                ? controller.profilePictureSource(endpointId) : defaultSource
    }

    implicitWidth: 32
    implicitHeight: 32

    function requestIfNeeded() {
        if (visible && controller && endpointId.length > 0) {
            controller.requestProfilePicture(endpointId)
        }
    }

    Component.onCompleted: Qt.callLater(requestIfNeeded)
    onVisibleChanged: Qt.callLater(requestIfNeeded)
    onEndpointIdChanged: Qt.callLater(requestIfNeeded)
    onControllerChanged: Qt.callLater(requestIfNeeded)
    Connections {
        target: root.controller
        ignoreUnknownSignals: true
        function onProfilesChanged() { Qt.callLater(root.requestIfNeeded) }
    }

    Item {
        anchors.fill: parent
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: circleMask
        }

        Image {
            objectName: "profilePictureFallback"
            anchors.fill: parent
            source: root.defaultSource
            visible: picture.status !== Image.Ready
            fillMode: Image.PreserveAspectCrop
            smooth: true
        }
        Image {
            id: picture
            objectName: "profilePictureImage"
            anchors.fill: parent
            source: root.resolvedSource
            visible: status === Image.Ready
            fillMode: Image.PreserveAspectCrop
            smooth: true
            mipmap: true
        }
    }

    Rectangle {
        id: circleMask
        anchors.fill: parent
        radius: Math.min(width, height) / 2
        color: "white"
        visible: false
        layer.enabled: true
    }
}

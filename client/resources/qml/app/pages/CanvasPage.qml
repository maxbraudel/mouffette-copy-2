import QtQuick
import Mouffette.App
import "../canvas"
import "../components"

AppPanel {
    id: root
    required property var controller
    readonly property var session: controller.activeWorkspace

    Loader {
        id: canvasLoader
        objectName: "activeCanvasLoader"
        anchors.fill: parent
        active: root.session !== null && root.session !== undefined
        source: Qt.resolvedUrl("../../CanvasRoot.qml")
        onLoaded: {
            item.shortcutScope = root
            if (root.session) item.sessionViewModel = root.session
        }
        onActiveChanged: if (!active && item) item.sessionViewModel = null
    }

    onSessionChanged: if (canvasLoader.item && root.session) {
        canvasLoader.item.sessionViewModel = root.session
    }

    AppSpinner {
        anchors.centerIn: parent
        visible: (!root.session && root.controller.remoteBusy)
            || (root.session && root.session.loading)
        running: visible
    }

    Text {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 640)
        visible: root.session && !root.session.loading && !root.session.hasScreens
        text: "No screens available"
        color: Theme.mutedText
        font.pixelSize: Math.max(28, Theme.titleFontSize * 1.5)
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        z: 99999
    }

    CanvasToolbar {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 10
        z: 100000
        visible: root.session !== null && root.session !== undefined
        session: root.session
    }

    SceneElementPanel {
        id: sceneElementPanel
        objectName: "canvasSceneElementPanel"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 10
        anchors.topMargin: 52
        z: 100001
        maximumHeight: Math.max(0, root.height - y - 10)
        session: root.session
    }

    MediaListPanel {
        objectName: "mediaListPanel"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        z: 100000
        session: root.session
    }
}

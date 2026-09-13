import QtQuick
import Mouffette.App
import "../canvas"
import "../components"

AppPanel {
    id: root
    required property var controller
    readonly property var session: controller.activeCanvasSession

    Loader {
        id: canvasLoader
        objectName: "activeCanvasLoader"
        anchors.fill: parent
        active: root.session !== null && root.session !== undefined
        source: Qt.resolvedUrl("../../CanvasRoot.qml")
        onLoaded: if (root.session) item.sessionViewModel = root.session
        onActiveChanged: if (!active && item) item.sessionViewModel = null
    }

    onSessionChanged: if (canvasLoader.item && root.session) {
        canvasLoader.item.sessionViewModel = root.session
    }

    AppSpinner {
        anchors.centerIn: parent
        visible: !root.session || root.session.loading
        running: visible
    }

    CanvasToolbar {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 10
        z: 100000
        visible: root.session !== null && root.session !== undefined
        session: root.session
    }

    Loader {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 10
        anchors.topMargin: 52
        z: 100001
        active: root.session !== null && root.session !== undefined
        sourceComponent: SceneElementPanel {
            session: root.session
        }
    }

    MediaListPanel {
        objectName: "mediaListPanel"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        z: 100000
        session: root.session
    }
}

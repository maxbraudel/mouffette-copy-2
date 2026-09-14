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
        onLoaded: if (root.session) item.sessionViewModel = root.session
        onActiveChanged: if (!active && item) item.sessionViewModel = null
    }

    onSessionChanged: if (canvasLoader.item && root.session) {
        canvasLoader.item.sessionViewModel = root.session
    }

    AppSpinner {
        anchors.centerIn: parent
        visible: root.session && root.session.loading
        running: visible
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 560)
        spacing: 18
        visible: !root.session

        Text {
            width: parent.width
            text: "No project has been created and no session has been launched"
            color: Theme.text
            font.pixelSize: Theme.titleFontSize
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10
            AppButton {
                text: "Create Project"
                enabled: root.controller.canCreateProject
                unavailableReason: "A project already exists for this client"
                onClicked: root.controller.createProject()
            }
            AppButton {
                text: "Launch Session"
                enabled: root.controller.canLaunchSession
                unavailableReason: root.controller.connectionEnabled
                    ? "The client is offline or unavailable"
                    : "Enable the local client first"
                onClicked: root.controller.launchSession()
            }
        }
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

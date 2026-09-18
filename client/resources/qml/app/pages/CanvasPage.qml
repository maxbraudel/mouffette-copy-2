import QtQuick
import QtQuick.Window
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
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: timelinePanel.top
        active: root.session !== null && root.session !== undefined
        source: Qt.resolvedUrl("../../CanvasRoot.qml")
        onLoaded: {
            item.shortcutScope = item
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

    // Scene playback owns the canvas. Unload editor controls completely so
    // they cannot render, retain focus, or remain exposed to accessibility.
    Loader {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 10
        z: 100000
        active: !!root.session && root.session.mediaEditingEnabled
        sourceComponent: CanvasToolbar {
            session: root.session
        }
    }

    Loader {
        id: settingsLoader
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 10
        anchors.topMargin: 52
        z: 100001
        active: !!root.session && root.session.mediaEditingEnabled
        sourceComponent: SceneElementPanel {
            id: sceneElementPanel
            objectName: "canvasSceneElementPanel"
            maximumHeight: Math.max(0, canvasLoader.height - settingsLoader.y - 10)
            session: root.session
            presentationReady: {
                var canvas = canvasLoader.item
                var media = canvas ? canvas.mediaDelegateById(sceneElementPanel.selectedMediaId) : null
                return !!media && media.initialFramePresented
            }
        }
    }

    MediaListPanel {
        objectName: "mediaListPanel"
        maximumHeight: Math.max(0, canvasLoader.height - 32)
        anchors.right: parent.right
        anchors.bottom: timelinePanel.top
        anchors.margins: 16
        z: 100000
        session: root.session
    }

    TimelinePanel {
        id: timelinePanel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: visible ? Math.min(implicitHeight, Math.max(120, root.height * 0.55)) : 0
        visible: !!root.session && root.session.hasProject
        session: root.session
    }

    Shortcut {
        sequence: "Space"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: !!root.session && !!root.session.timeline && !root.session.timeline.remoteActive
            && root.visible && (function() {
                var item = root.Window.window ? root.Window.window.activeFocusItem : null
                while (item) {
                    if (item instanceof TextInput || item instanceof TextEdit) return false
                    if (item === root) return true
                    item = item.parent
                }
                return false
            })()
        onActivated: root.session.timeline.togglePlayback()
    }
}

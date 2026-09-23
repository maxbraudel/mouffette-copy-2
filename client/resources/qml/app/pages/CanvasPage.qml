import QtQuick
import QtQuick.Window
import Mouffette.App
import "../.." as Canvas
import "../canvas"
import "../components"

AppPanel {
    id: root
    required property var controller
    readonly property var session: controller.activeWorkspace
    readonly property real overlaySpacing: 10
    property bool timelineExpanded: true
    property real timelineHeightRatio: 0.5
    readonly property var timeline: session ? session.timeline : null
    readonly property real minimumTimelineRatio: timeline ? timeline.timelineMinHeightPercent / 100 : 0.2
    readonly property real maximumTimelineRatio: 1 - (timeline ? timeline.canvasMinHeightPercent / 100 : 0.2)
    readonly property real splitHeight: Math.max(0, height - border.width * 2 - canvasTimelineSeparator.height)
    readonly property bool shortcutsAvailable: visible && !!session && session.hasProject && (function() {
        var item = root.Window.window ? root.Window.window.activeFocusItem : null
        while (item) {
            if (item instanceof TextInput || item instanceof TextEdit) return false
            if (item === root) return true
            item = item.parent
        }
        return false
    })()
    color: Theme.canvasBackground

    Loader {
        id: canvasLoader
        objectName: "activeCanvasLoader"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.border.width
        anchors.rightMargin: root.border.width
        anchors.topMargin: root.border.width
        anchors.bottom: canvasTimelineSeparator.top
        active: root.session !== null && root.session !== undefined
        sourceComponent: Canvas.CanvasRoot {
            // The page owns the shared background and rounded frame.
            color: "transparent"
            border.width: 0
            radius: 0
        }
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
        anchors.centerIn: canvasLoader
        visible: (!root.session && root.controller.remoteStatusText === "CONNECTING")
            || (root.session && root.session.loading)
        running: visible
    }

    // Timeline visibility remains available during playback. The toolbar
    // unloads its editing controls independently when the scene owns the canvas.
    Loader {
        id: toolbarLoader
        anchors.left: canvasLoader.left
        anchors.top: canvasLoader.top
        anchors.margins: root.overlaySpacing
        z: 100000
        active: !!root.session
        sourceComponent: CanvasToolbar {
            session: root.session
            controller: root.controller
            timelineExpanded: root.timelineExpanded
            onToggleTimeline: root.timelineExpanded = !root.timelineExpanded
        }
    }

    Loader {
        id: settingsLoader
        anchors.left: toolbarLoader.left
        anchors.top: toolbarLoader.bottom
        anchors.topMargin: root.overlaySpacing
        z: 100001
        active: !!root.session && root.session.mediaEditingEnabled
        sourceComponent: SceneElementPanel {
            id: sceneElementPanel
            objectName: "canvasSceneElementPanel"
            maximumHeight: Math.max(0, canvasLoader.y + canvasLoader.height
                - settingsLoader.y - root.overlaySpacing)
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
        maximumHeight: Math.max(0, canvasLoader.height - root.overlaySpacing * 2)
        anchors.right: canvasLoader.right
        anchors.bottom: canvasLoader.bottom
        anchors.margins: root.overlaySpacing
        z: 100000
        session: root.session
    }

    Rectangle {
        id: canvasTimelineSeparator
        anchors.left: timelinePanel.left
        anchors.right: timelinePanel.right
        anchors.bottom: timelinePanel.top
        height: visible ? 1 : 0
        visible: timelinePanel.visible
        color: Theme.border
    }

    MouseArea {
        id: timelineSplitter
        objectName: "canvasTimelineSplitter"
        anchors.left: canvasTimelineSeparator.left
        anchors.right: canvasTimelineSeparator.right
        anchors.verticalCenter: canvasTimelineSeparator.verticalCenter
        height: root.timeline ? root.timeline.timelineSplitterHitHeightPx : 12
        z: 100002
        enabled: timelinePanel.visible && root.timelineExpanded
        visible: enabled
        hoverEnabled: true
        cursorShape: Qt.SizeVerCursor
        preventStealing: true
        property real pressY: 0
        property real initialTimelineHeight: 0
        onPressed: mouse => {
            pressY = mapToItem(root, mouse.x, mouse.y).y
            initialTimelineHeight = timelinePanel.height
        }
        onPositionChanged: mouse => {
            if (!pressed || root.splitHeight <= 0) return
            var delta = mapToItem(root, mouse.x, mouse.y).y - pressY
            root.timelineHeightRatio = Math.max(root.minimumTimelineRatio,
                Math.min(root.maximumTimelineRatio, (initialTimelineHeight - delta) / root.splitHeight))
        }
    }

    TimelinePanel {
        id: timelinePanel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: root.border.width
        height: visible ? (expanded ? root.splitHeight * Math.max(root.minimumTimelineRatio,
                                       Math.min(root.maximumTimelineRatio, root.timelineHeightRatio))
                                   : transportHeight) : 0
        expanded: root.timelineExpanded
        bottomCornerRadius: Math.max(0, root.radius - root.border.width)
        visible: !!root.session && root.session.hasProject
        session: root.session
    }

    Shortcut {
        sequence: "Tab"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: root.shortcutsAvailable && !timelineSplitter.pressed
        onActivated: root.timelineExpanded = !root.timelineExpanded
    }

    Shortcut {
        sequence: "Space"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: root.shortcutsAvailable && !!root.timeline && !root.timeline.remoteActive
        onActivated: root.session.timeline.togglePlayback()
    }
}

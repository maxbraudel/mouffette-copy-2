import QtQuick
import "../.." as CanvasControls

Row {
    id: root
    required property var session
    property var controller: null
    property bool timelineExpanded: true
    readonly property bool editingControlsAvailable: !!session && session.mediaEditingEnabled
    signal toggleTimeline()
    spacing: 8

    Loader {
        active: root.editingControlsAvailable
        visible: active && root.session.hasProject
        sourceComponent: CanvasControls.OverlayButton {
            objectName: "canvasSettingsButton"
            iconSource: "qrc:/icons/icons/settings.svg"
            accessibleName: "Settings"
            isToggle: true
            toggled: root.session && root.session.settingsVisible
            visible: root.session && root.session.hasProject
            enabled: root.session && root.session.mediaEditingEnabled
            unavailableReason: root.session ? root.session.mediaEditingUnavailableReason : "Canvas is unavailable"
            onClicked: root.session.settingsVisible = !root.session.settingsVisible
        }
    }
    CanvasControls.OverlayButton {
        objectName: "canvasTimelineButton"
        iconSource: "qrc:/icons/icons/timeline/panel.svg"
        accessibleName: "Timeline"
        isToggle: true
        toggled: root.timelineExpanded
        visible: root.session && root.session.hasProject
        onClicked: root.toggleTimeline()
    }
    Loader {
        active: root.editingControlsAvailable
        visible: active && root.session.hasProject
        sourceComponent: Row {
            visible: root.session && root.session.hasProject
            spacing: 0
            CanvasControls.OverlayButton {
                objectName: "canvasSelectionToolButton"
                iconSource: "qrc:/icons/icons/tools/selection-tool.svg"
                accessibleName: "Selection tool"
                isToggle: true
                toggled: !root.session || root.session.activeTool === "selection"
                segmentRole: "leading"
                enabled: root.session !== null && root.session !== undefined
                unavailableReason: root.session ? root.session.canvasNavigationUnavailableReason : "Canvas is unavailable"
                onClicked: root.session.setActiveTool("selection")
            }
            CanvasControls.OverlayButton {
                objectName: "canvasTextToolButton"
                iconSource: "qrc:/icons/icons/tools/text-tool.svg"
                accessibleName: "Text tool"
                isToggle: true
                toggled: root.session && root.session.activeTool === "text"
                segmentRole: "trailing"
                visible: root.session && root.session.hasProject
                enabled: root.session && root.session.textCreation
                unavailableReason: root.session ? root.session.textCreationUnavailableReason : "Canvas is unavailable"
                onClicked: root.session.setActiveTool("text")
            }
        }
    }
    CanvasControls.OverlayButton {
        objectName: "canvasRemoteAudioButton"
        iconSource: root.controller && root.controller.remoteAudioMuted
            ? "qrc:/icons/icons/volume-off.svg" : "qrc:/icons/icons/volume-on.svg"
        accessibleName: root.controller && root.controller.remoteAudioMuted
            ? "Unmute remote audio" : "Mute remote audio"
        Accessible.description: (root.controller && root.controller.remoteAudioState) || ""
        isToggle: true
        toggled: root.controller && !root.controller.remoteAudioMuted
        enabled: !!root.controller && !!root.session
        onClicked: root.controller.setRemoteAudioMuted(!root.controller.remoteAudioMuted)
    }
}

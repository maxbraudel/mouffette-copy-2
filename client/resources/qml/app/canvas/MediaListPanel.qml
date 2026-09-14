import QtQuick
import QtQuick.Controls
import Mouffette.App
import "../components"

Rectangle {
    id: root

    required property var session
    readonly property var mediaModel: session ? session.mediaModel : null
    readonly property int mediaCount: session ? session.mediaCount : 0
    readonly property int mediaRowHeight: 54
    readonly property int actionAreaHeight: Theme.overlayButtonHeight * 3

    visible: session && session.hasProject
    enabled: visible
    width: Math.min(420, Math.max(220, implicitWidth))
    height: Math.min(parent ? Math.max(0, parent.height - 20)
                            : Math.max(1, mediaCount) * mediaRowHeight + actionAreaHeight,
                     (mediaCount > 0 ? mediaCount * mediaRowHeight : 0) + actionAreaHeight)
    radius: Theme.overlayRadius
    color: Theme.overlayBackground
    border.width: 1
    border.color: Theme.overlayBorder
    clip: true

    Column {
        anchors.fill: parent
        spacing: 0

        ListView {
            id: list
            objectName: "mediaList"
            width: parent.width
            height: Math.max(0, root.height - root.actionAreaHeight)
            visible: root.mediaCount > 0
            model: root.mediaModel
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: ItemDelegate {
                id: row
                objectName: "mediaRow_" + index
                required property int index
                required property string mediaId
                required property string displayName
                required property string mediaType
                required property string uploadState
                required property var modelData
                width: list.width
                height: root.mediaRowHeight
                hoverEnabled: true
                background: Rectangle {
                    color: row.hovered ? Qt.rgba(1, 1, 1, 0.05) : "transparent"
                }
                contentItem: Column {
                    id: details
                    leftPadding: 20
                    rightPadding: 20
                    spacing: 3
                    Text {
                        width: row.width - 40
                        text: row.displayName
                        color: Theme.overlayText
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        width: row.width - 40
                        text: row.mediaType
                        visible: text.length > 0
                        color: Theme.overlayText
                        opacity: 0.75
                        elide: Text.ElideRight
                    }
                    ProgressBar {
                        width: row.width - 40
                        visible: row.uploadState === "uploading"
                        from: 0
                        to: 100
                        value: modelData.uploadProgress || 0
                    }
                }
                onClicked: root.session.selectMedia(row.mediaId, false)
            }
        }

        Column {
            id: actions
            width: parent.width
            height: root.actionAreaHeight
            spacing: 0

            OverlayActionButton {
                width: actions.width
                height: Theme.overlayButtonHeight
                text: root.session ? root.session.remoteSceneActionText : "Launch Remote Scene"
                tone: OverlayActionButton.Remote
                enabled: root.session && root.session.remoteSceneActionEnabled
                unavailableReason: root.session ? root.session.remoteSceneUnavailableReason : ""
                onClicked: root.session.toggleRemoteScene()
            }
            OverlayActionButton {
                width: actions.width
                height: Theme.overlayButtonHeight
                text: root.session ? root.session.testSceneActionText : "Launch Test Scene"
                tone: OverlayActionButton.Test
                enabled: root.session && root.session.testSceneActionEnabled
                unavailableReason: root.session ? root.session.testSceneUnavailableReason : ""
                onClicked: root.session.toggleTestScene()
            }
            OverlayActionButton {
                width: actions.width
                height: Theme.overlayButtonHeight
                text: root.session ? root.session.uploadActionText : "Upload"
                tone: root.session ? root.session.uploadActionTone : OverlayActionButton.Normal
                enabled: root.session && root.session.uploadActionEnabled
                unavailableReason: root.session ? root.session.uploadUnavailableReason : ""
                onClicked: root.session.triggerUploadAction()
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Mouffette.App
import Mouffette.Canvas
import "../components"

Rectangle {
    id: root

    required property var session
    readonly property var mediaModel: session ? session.mediaModel : null
    readonly property int mediaCount: session ? session.mediaCount : 0
    readonly property int actionAreaHeight: (Theme.overlayButtonHeight + 1) * 2
    property real mediaNaturalWidth: 0
    property real maximumHeight: parent ? Math.max(0, parent.height - 32) : implicitHeight

    function measureMediaWidth() {
        var measured = 0
        for (var i = 0; i < mediaRows.count; ++i) {
            var row = mediaRows.itemAt(i)
            if (row) measured = Math.max(measured, row.implicitWidth)
        }
        mediaNaturalWidth = measured
    }

    function humanSize(bytes) {
        if (bytes === undefined || bytes < 0) return "n/a"
        var units = ["B", "KB", "MB", "GB"]
        var unit = 0
        while (bytes >= 1024 && unit < 3) { bytes /= 1024; ++unit }
        return bytes.toFixed(unit === 0 ? 0 : bytes < 10 ? 2 : 1) + " " + units[unit]
    }

    visible: session && session.hasProject
    implicitWidth: Math.max(200, mediaNaturalWidth, remoteButton.implicitWidth,
                            uploadButton.implicitWidth)
    width: Math.min(implicitWidth, 420, parent ? Math.max(0, parent.width * 0.5) : 420)
    implicitHeight: mediaColumn.height + actionAreaHeight
    height: Math.min(implicitHeight, maximumHeight)
    radius: Theme.overlayRadius
    color: Theme.overlayBackground

    // Keep disabled controls and separators from forwarding presses or wheel
    // events to the canvas. The interactive children sit above this shield.
    MouseArea {
        z: 0
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => { wheel.accepted = true }
    }

    Item {
        id: panelContent
        z: 1
        anchors.fill: parent
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: roundedMask
        }

        Flickable {
            id: list
            objectName: "mediaList"
            readonly property int count: mediaRows.count
            width: parent.width
            height: Math.max(0, root.height - root.actionAreaHeight)
            contentWidth: width
            contentHeight: mediaColumn.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            onContentYChanged: scrollbarHide.restart()

            Column {
                id: mediaColumn
                width: list.width
                spacing: 0

                Repeater {
                    id: mediaRows
                    model: root.mediaModel
                    onItemAdded: Qt.callLater(root.measureMediaWidth)
                    onItemRemoved: Qt.callLater(root.measureMediaWidth)

                    delegate: Control {
                        id: row
                        objectName: "mediaRow_" + index
                        required property int index
                        required property string displayName
                        required property string mediaType
                        required property string uploadState
                        required property var modelData
                        readonly property bool textMedia: mediaType === "text"
                        readonly property bool uploadedAndCached: uploadState === "uploaded" && !!modelData.remoteCached
                        readonly property bool awaitingRemoteCache: uploadState === "uploaded" && !uploadedAndCached
                        readonly property bool showProgress: uploadState === "uploading" || awaitingRemoteCache
                        readonly property string statusText: uploadedAndCached ? "Uploaded and Cached"
                            : uploadState === "uploaded" ? "Uploaded" : "Not uploaded"
                        readonly property string dimensions: Math.round(modelData.width || 0)
                            + " x " + Math.round(modelData.height || 0) + " px"
                        readonly property string detailsText: dimensions
                            + (textMedia ? "" : "  ·  " + root.humanSize(modelData.sourceSizeBytes))
                        implicitWidth: Math.max(nameMetrics.advanceWidth, detailsMetrics.advanceWidth,
                                                textMedia ? 0 : statusMetrics.maximumWidth) + 40
                        width: mediaColumn.width
                        height: details.implicitHeight + 16 + (index > 0 ? 1 : 0)
                        padding: 0
                        leftPadding: 20
                        rightPadding: 20
                        topPadding: 8 + (index > 0 ? 1 : 0)
                        bottomPadding: 8
                        hoverEnabled: false
                        focusPolicy: Qt.NoFocus
                        onImplicitWidthChanged: Qt.callLater(root.measureMediaWidth)
                        Accessible.name: displayName
                        Accessible.description: (textMedia ? "" : (uploadState === "uploading" ? "Uploading"
                            : awaitingRemoteCache ? "Uploaded, preparing remote cache" : statusText) + ", ") + detailsText
                        background: Rectangle {
                            color: "transparent"
                        }
                        Rectangle {
                            width: parent.width
                            height: 1
                            visible: row.index > 0
                            color: Theme.overlayBorder
                        }
                        TextMetrics { id: nameMetrics; text: row.displayName; font: nameLabel.font }
                        TextMetrics { id: detailsMetrics; text: row.detailsText; font: detailLabel.font }
                        StateTextMetrics {
                            id: statusMetrics
                            text: row.statusText
                            textVariants: ["Not uploaded", "Uploaded", "Uploaded and Cached"]
                            font: statusLabel.font
                        }
                        contentItem: Column {
                            id: details
                            spacing: 3
                            Text {
                                id: nameLabel
                                objectName: "mediaName_" + row.index
                                width: parent.width
                                height: Math.max(18, Math.ceil(implicitHeight) + 2)
                                text: row.displayName
                                textFormat: Text.PlainText
                                color: Theme.overlayText
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            Item {
                                width: parent.width
                                height: 20
                                visible: !row.textMedia
                                Text {
                                    id: statusLabel
                                    objectName: "mediaStatus_" + row.index
                                    anchors.fill: parent
                                    text: row.statusText
                                    visible: !row.showProgress
                                    color: row.uploadedAndCached ? Theme.mediaUploaded : Theme.mediaNotUploaded
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    objectName: "mediaProgress_" + row.index
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width
                                    height: 10
                                    visible: row.showProgress
                                    color: Theme.mediaProgressBackground
                                    Rectangle {
                                        id: progressFill
                                        objectName: "mediaProgressFill_" + row.index
                                        height: parent.height
                                        width: row.awaitingRemoteCache ? parent.width
                                            : parent.width * Math.max(0, Math.min(100, row.modelData.uploadProgress || 0)) / 100
                                        color: Theme.mediaProgress
                                        SequentialAnimation on opacity {
                                            running: row.awaitingRemoteCache && progressFill.visible
                                            loops: Animation.Infinite
                                            onRunningChanged: if (!running) progressFill.opacity = 1
                                            NumberAnimation { from: 1; to: 0.45; duration: 700; easing.type: Easing.InOutSine }
                                            NumberAnimation { from: 0.45; to: 1; duration: 700; easing.type: Easing.InOutSine }
                                        }
                                    }
                                }
                            }
                            Text {
                                id: detailLabel
                                objectName: "mediaDetails_" + row.index
                                width: parent.width
                                height: Math.max(18, Math.ceil(implicitHeight) + 2)
                                text: row.detailsText
                                textFormat: Text.PlainText
                                color: Theme.overlaySecondaryText
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {
                id: scrollbar
                objectName: "mediaScrollBar"
                parent: panelContent
                x: root.width - width - 6
                y: 6
                width: 8
                height: Math.max(0, list.height - 12)
                padding: 0
                minimumSize: height > 0 ? Math.min(1, 24 / height) : 1
                policy: ScrollBar.AlwaysOn
                visible: list.contentHeight > list.height && (scrollbarHide.running || pressed)
                hoverEnabled: true
                contentItem: Rectangle {
                    radius: 4
                    color: scrollbar.pressed ? Theme.scrollbarPressed
                         : scrollbar.hovered ? Theme.scrollbarHover : Theme.scrollbar
                }
                background: Item {}
                onPressedChanged: scrollbarHide.restart()
            }
        }

        Column {
            id: actions
            y: Math.max(0, root.height - height)
            width: parent.width
            height: root.actionAreaHeight
            spacing: 0

            Rectangle { width: parent.width; height: 1; color: Theme.overlayBorder }
            OverlayActionButton {
                id: remoteButton
                objectName: "remoteSceneAction"
                width: actions.width
                text: root.session ? root.session.remoteSceneActionText : "Launch Remote Scene"
                textVariants: ["Launch Remote Scene", "Launching Remote Scene...",
                               "Stop Remote Scene", "Stopping Remote Scene..."]
                tone: root.session ? root.session.remoteSceneActionTone : OverlayActionButton.Normal
                busy: tone === OverlayActionButton.Uploading && !root.session.actionPending
                enabled: !!root.session
                unavailableReason: root.session ? root.session.remoteSceneUnavailableReason : ""
                onClicked: root.session.toggleRemoteScene()
            }
            Rectangle { width: parent.width; height: 1; color: Theme.overlayBorder }
            OverlayActionButton {
                id: uploadButton
                objectName: "uploadAction"
                width: actions.width
                text: root.session ? root.session.uploadActionText : "Upload"
                textVariants: ["Upload", "Unload", "Preparing…", "Uploading…",
                               "Finalizing…", "Cancelling…", "Removing…"]
                // Reserve upload counters from the number of unique sources. Keep all
                // counter digits before upload starts, in its progress font.
                monospaceTextVariants: {
                    var digits = "9".repeat(String(Math.max(1, root.mediaCount)).length)
                    return ["Uploading (" + digits + "/" + digits + ") 100%"]
                }
                tone: root.session ? root.session.uploadActionTone : OverlayActionButton.Normal
                busy: tone === OverlayActionButton.Uploading && !root.session.actionPending
                monospace: busy && root.session.uploadActionEnabled
                enabled: !!root.session
                unavailableReason: root.session ? root.session.uploadUnavailableReason : ""
                bottomRadius: Theme.overlayRadius
                onClicked: root.session.triggerUploadAction()
            }
        }
    }

    Rectangle {
        id: roundedMask
        z: -1
        anchors.fill: parent
        radius: root.radius
        // Alpha mask only; this is not a visible surface.
        color: "white"
        visible: false
        layer.enabled: true
    }
    Rectangle {
        z: 2
        objectName: "mediaPanelBorder"
        anchors.fill: parent
        color: "transparent"
        radius: root.radius
        border.width: 1
        border.color: Theme.overlayBorder
    }
    Timer { id: scrollbarHide; interval: UiTiming.scrollbarHideDelayMs }
}

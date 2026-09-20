import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mouffette.Canvas
import Mouffette.App
import "../components"

Popup {
    id: root
    objectName: "memoryUsagePopup"
    parent: Overlay.overlay
    width: Math.min(720, parent ? parent.width - 32 : 720)
    height: Math.min(680, parent ? parent.height - 32 : 680)
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    modal: true
    focus: true
    palette: Theme.controlPalette
    Overlay.modal: Rectangle { color: Theme.modalScrim }
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: assetList.positionViewAtBeginning()
    readonly property var usage: MediaMemory.summary
    readonly property real total: Math.max(1, usage.totalBytes || 1)
    readonly property color secondaryText: Theme.mutedText

    function bytes(value) {
        var amount = Number(value || 0)
        if (amount >= 1073741824) return (amount / 1073741824).toFixed(2) + " GiB"
        if (amount >= 1048576) return (amount / 1048576).toFixed(1) + " MiB"
        if (amount >= 1024) return (amount / 1024).toFixed(1) + " KiB"
        return Math.round(amount) + " B"
    }
    function stateLabel(entry) {
        var state = String(entry.state || "queued").toLowerCase()
        if (state === "ready") return "Ready"
        if (state === "error") return "Error"
        if (state === "analysing") return "Reading media information"
        if (state === "capacity_insufficient") return "Media exceeds available capacity"
        if (state.indexOf("memory") >= 0 || state === "evicted") return "Waiting for memory"
        if (state === "loading" || state === "decoding")
            return "Loading " + Math.round((entry.progress || 0) * 100) + "%"
        return "Queued"
    }

    background: Rectangle {
        color: Theme.elevatedBackground
        border.color: Theme.border
        radius: 10
    }
    contentItem: ColumnLayout {
        spacing: 14
        RowLayout {
            Layout.fillWidth: true
            Text { text: "RAM usage"; color: Theme.text; font.pixelSize: 18; font.bold: true }
            Item { Layout.fillWidth: true }
            AppButton { text: "Close"; onClicked: root.close() }
        }
        // A single scrolling surface keeps the breakdown and asset details
        // reachable even in short windows or at a large display scale.
        ListView {
            id: assetList
            objectName: "memoryAssetList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            // This informational list has no selected asset.
            currentIndex: -1
            // The header grows when its cards wrap. Keep the same offset from
            // the top as ListView moves its origin to accommodate that height.
            property real previousOriginY: 0
            onOriginYChanged: {
                contentY += originY - previousOriginY
                previousOriginY = originY
            }
            model: MediaMemory.assets
            ScrollBar.vertical: ScrollBar {}
            header: ColumnLayout {
                width: assetList.width
                spacing: 12
                Text {
                    Layout.fillWidth: true
                    objectName: "memoryStoredTotal"
                    text: root.bytes(root.usage.mediaBytes) + " stored in media RAM"
                    color: Theme.text
                    font.pixelSize: 15
                    font.bold: true
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: width >= 680 ? 5 : 2
                    uniformCellWidths: true
                    columnSpacing: 8; rowSpacing: 8
                    Repeater {
                        model: [
                            {key: "videoBytes", label: "Video data", detail: "Original compressed video"},
                            {key: "imageBytes", label: "Images", detail: "Decoded image pixels"},
                            {key: "posterBytes", label: "Video preview frames", detail: "Full-size first frames"},
                            {key: "thumbnailBytes", label: "Timeline thumbnails", detail: "Small clip previews"},
                            {key: "scrubProxyBytes", label: "Scrubbing previews", detail: "Compressed editing images"}
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            objectName: "memoryCategory_" + modelData.key
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            implicitHeight: metric.implicitHeight + 20
                            color: Theme.surfaceBackground
                            radius: 5
                            ColumnLayout {
                                id: metric
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.top: parent.top; anchors.margins: 10
                                spacing: 5
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.label
                                    color: Theme.text; font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                }
                                Text {
                                    Layout.fillWidth: true
                                    objectName: "memoryAmount_" + modelData.key
                                    text: root.bytes(root.usage[modelData.key])
                                    color: Theme.text; font.pixelSize: 18; font.bold: true
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.detail
                                    color: root.secondaryText; font.pixelSize: 10
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Shared media is counted once, even when used by several clips."
                    color: root.secondaryText; font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: playbackDetails.implicitHeight + 20
                    color: Theme.surfaceBackground
                    radius: 5
                    ColumnLayout {
                        id: playbackDetails
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.top: parent.top; anchors.margins: 10
                        spacing: 5
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: "Playback (estimated)"
                                color: Theme.text; font.pixelSize: 12; font.bold: true
                                wrapMode: Text.Wrap
                            }
                            Text {
                                objectName: "memoryPlaybackEstimate"
                                text: root.bytes(root.usage.playbackBudgetBytes)
                                color: Theme.text; font.pixelSize: 15; font.bold: true
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "Budget for video and audio decoding. This is not a measured allocation and is not added to the stored-media total."
                            color: root.secondaryText; font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Text {
                            Layout.fillWidth: true
                            objectName: "memoryPendingPlayback"
                            text: root.bytes(root.usage.pendingPlaybackBudgetBytes) + " still reserved for players preparing"
                            color: root.secondaryText; font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    objectName: "memoryLoadableBudget"
                    text: root.bytes(root.usage.loadableBytes) + " available for new media · "
                        + root.bytes(root.usage.reserveBytes) + " kept available for the system\n"
                        + root.bytes(root.usage.reservedBytes) + " reserved for media loading"
                    color: Theme.text; font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    objectName: "memoryPressureStatus"
                    visible: root.usage.pressure === "warning" || root.usage.pressure === "critical"
                    text: root.usage.pressure === "critical"
                        ? "Critical memory pressure: loading is paused and media may be released."
                        : "System memory warning: loading continues when its full preparation budget fits."
                    color: root.usage.pressure === "critical" ? Theme.errorText : Theme.warningText
                    font.pixelSize: 12; wrapMode: Text.Wrap
                }
                Rectangle {
                    id: ramBar
                    objectName: "memoryDistributionBar"
                    Layout.fillWidth: true
                    implicitHeight: 20
                    color: Theme.chartAvailable
                    radius: 4; clip: true
                    Row {
                        anchors.fill: parent
                        Rectangle {
                            height: parent.height
                            width: ramBar.width * Math.min(1, (root.usage.processBytes || 0) / root.total)
                            color: Theme.chartProcess
                        }
                        Rectangle {
                            height: parent.height
                            width: ramBar.width * Math.min(1, (root.usage.otherBytes || 0) / root.total)
                            color: Theme.chartOther
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 12
                    Repeater {
                        model: [
                            {label: "Mouffette", amount: root.usage.processBytes, tint: Theme.chartProcess},
                            {label: "System and other apps", amount: root.usage.otherBytes, tint: Theme.chartOther},
                            {label: "Available" + (root.usage.availableEstimated ? " (estimated)" : ""),
                             amount: root.usage.availableBytes, tint: Theme.chartAvailable}
                        ]
                        delegate: Row {
                            required property var modelData
                            spacing: 5
                            Rectangle { width: 8; height: 8; radius: 2; color: modelData.tint; y: 3 }
                            Text {
                                text: modelData.label + "  " + root.bytes(modelData.amount)
                                color: Theme.text; font.pixelSize: 11
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Mouffette's total includes app and playback allocations. Playback and graphics memory are not measured separately per media."
                    color: root.secondaryText; font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    text: "Media from all open canvases and remote sessions"
                    color: Theme.text; font.pixelSize: 13; font.bold: true
                    wrapMode: Text.Wrap
                    Layout.bottomMargin: 8
                }
            }
            delegate: Rectangle {
                required property var modelData
                width: assetList.width
                height: details.implicitHeight + 20
                color: Theme.surfaceBackground
                radius: 5
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10
                    ColumnLayout {
                        id: details
                        Layout.fillWidth: true
                        spacing: 5
                        Text {
                            Layout.fillWidth: true
                            text: modelData.displayName || "Media"
                            color: Theme.text; font.pixelSize: 13
                            elide: Text.ElideMiddle
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.stateLabel(modelData)
                                + (modelData.occurrences > 1 ? " · Shared by " + modelData.occurrences + " uses" : "")
                                + (modelData.protected ? " · In scene" : "")
                            color: root.secondaryText; font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Text {
                            Layout.fillWidth: true
                            objectName: "memoryAssetBreakdown"
                            text: (modelData.isVideo
                                ? "Video data " + root.bytes(modelData.videoBytes)
                                    + " · Preview frame " + root.bytes(modelData.posterBytes)
                                : "Image pixels " + root.bytes(modelData.imageBytes))
                                + " · Thumbnails " + root.bytes(modelData.thumbnailBytes)
                                + (modelData.isVideo ? " · Scrubbing " + root.bytes(modelData.scrubProxyBytes) : "")
                                + "\nStored total " + root.bytes(modelData.residentBytes)
                                + (modelData.isVideo ? " · Playback estimate " + root.bytes(modelData.playbackBudgetBytes) : "")
                            color: Theme.text; font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: [(modelData.state !== "ready" && modelData.estimatedBytes > 0
                                ? "Expected media storage " + root.bytes(modelData.estimatedBytes)
                                    + " · Loading reserve " + root.bytes(modelData.reservedBytes) : ""),
                                (modelData.pendingPlaybackBudgetBytes > 0
                                    ? "Player preparation reserve " + root.bytes(modelData.pendingPlaybackBudgetBytes) : "")]
                                .filter(value => value.length > 0).join(" · ")
                            color: root.secondaryText; font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: modelData.error || ""
                            color: Theme.errorText; font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            model: modelData.remoteStates || []
                            delegate: Text {
                                required property var modelData
                                Layout.fillWidth: true
                                text: "Remote " + (modelData.targetName || String(modelData.targetId || "").slice(0, 8))
                                    + ": " + root.stateLabel(modelData)
                                    + (modelData.error ? " — " + modelData.error : "")
                                color: modelData.state === "error" ? Theme.errorText : root.secondaryText
                                font.pixelSize: 11; wrapMode: Text.Wrap
                            }
                        }
                    }
                    AppButton {
                        text: "Retry"
                        visible: String(modelData.state).toLowerCase() === "error"
                        onClicked: MediaMemory.retry(modelData.ownerId)
                    }
                }
            }
            footer: Text {
                width: assetList.width
                visible: assetList.count === 0
                height: visible ? implicitHeight + 16 : 0
                text: "No media loaded"
                horizontalAlignment: Text.AlignHCenter
                color: root.secondaryText
            }
        }
    }
}

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
    height: Math.min(560, parent ? parent.height - 32 : 560)
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    modal: true
    focus: true
    palette: Theme.controlPalette
    Overlay.modal: Rectangle { color: Theme.modalScrim }
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: {
        assetList.positionViewAtBeginning()
        Qt.callLater(function() { if (root.opened) assetList.positionViewAtBeginning() })
    }
    readonly property var usage: MediaMemory.summary
    readonly property var assetEntries: MediaMemory.assets
    readonly property real total: Math.max(1, usage.totalBytes || 1)
    // The OS counters are estimates. Keep the historical distribution bounded
    // by physical RAM even when its process/availability measurements overlap.
    readonly property real availableRam: Math.max(0, Math.min(total, Number(usage.availableBytes || 0)))
    readonly property real processRam: Math.max(0, Math.min(total - availableRam, Number(usage.processBytes || 0)))
    readonly property real otherRam: Math.max(0, total - availableRam - processRam)
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
            readonly property real rowWidth: Math.max(0, width
                - (contentHeight > height ? memoryScrollBar.width + 8 : 0))
            // This informational list has no selected asset.
            currentIndex: -1
            // Keep the summary in the same coordinate system as asset rows.
            // A separate inline header can retain a stale negative offset when
            // the live model changes while the popup is resized or reopened.
            model: root.assetEntries.length + 1
            delegate: Loader {
                required property int index
                readonly property var entry: index > 0 ? root.assetEntries[index - 1] : null
                width: assetList.rowWidth
                height: item ? item.implicitHeight : 0
                sourceComponent: index === 0 ? summaryRow : assetRow
            }
            ScrollBar.vertical: ScrollBar { id: memoryScrollBar }
            Component {
                id: summaryRow
                ColumnLayout {
                    width: assetList.rowWidth
                    spacing: 12
                    Text {
                        Layout.fillWidth: true
                        objectName: "memoryStoredTotal"
                        text: root.bytes(root.usage.sharedStoredBytes) + " retained in shared media · "
                            + root.bytes(root.usage.totalBytes) + " total RAM"
                        color: root.secondaryText
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        objectName: "memoryLoadableBudget"
                        text: root.bytes(root.usage.loadableBytes) + " available for new media · "
                            + root.bytes(root.usage.reserveBytes) + " kept available for the system"
                        color: Theme.text; font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        objectName: "memoryPendingPlayback"
                        text: root.bytes(root.usage.reservedBytes) + " pending media preparation · "
                            + root.bytes(root.usage.pendingPlaybackBudgetBytes) + " pending cursor preparation"
                        color: root.secondaryText; font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Text {
                            Layout.fillWidth: true
                            text: "Graphics and decoding (estimated)"
                            color: root.secondaryText; font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Text {
                            objectName: "memoryPlaybackEstimate"
                            text: root.bytes(root.usage.playbackBudgetBytes)
                            color: root.secondaryText; font.pixelSize: 11
                        }
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
                        implicitHeight: 26
                        color: Theme.chartAvailable
                        radius: 4
                        clip: true
                        Row {
                            anchors.fill: parent
                            Rectangle {
                                objectName: "memoryProcessSegment"
                                height: parent.height
                                width: ramBar.width * root.processRam / root.total
                                color: Theme.chartProcess
                            }
                            Rectangle {
                                objectName: "memoryOtherSegment"
                                height: parent.height
                                width: ramBar.width * root.otherRam / root.total
                                color: Theme.chartOther
                            }
                            Rectangle {
                                objectName: "memoryAvailableSegment"
                                height: parent.height
                                width: ramBar.width * root.availableRam / root.total
                                color: Theme.chartAvailable
                            }
                        }
                    }
                    Flow {
                        objectName: "memoryDistributionLegend"
                        Layout.fillWidth: true
                        spacing: 12
                        Repeater {
                            model: [
                                {key: "process", label: "Mouffette", amount: root.processRam, tint: Theme.chartProcess},
                                {key: "other", label: "System and other apps", amount: root.otherRam, tint: Theme.chartOther},
                                {key: "available", label: "Available" + (root.usage.availableEstimated ? " (estimated)" : ""),
                                 amount: root.availableRam, tint: Theme.chartAvailable}
                            ]
                            delegate: Row {
                                required property var modelData
                                objectName: "memoryLegend_" + modelData.key
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
                        text: "Estimated distribution based on available RAM and Mouffette's memory footprint. Shared media is counted once."
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
            }
            Component {
                id: assetRow
                Rectangle {
                    property var modelData: parent && parent.entry ? parent.entry : ({})
                    width: assetList.rowWidth
                    implicitHeight: details.implicitHeight + 20
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

                                    + (modelData.audioPreviewBytes ? " · Initial audio " + root.bytes(modelData.audioPreviewBytes) : "")
                                    + "\nStored total " + root.bytes(Number(modelData.videoBytes || 0) + Number(modelData.imageBytes || 0))
                                    + (modelData.isVideo ? " · " + (modelData.allIntra ? "H.264 intra" : "Original format") : "")
                                    + (modelData.sourceFileBytes ? " · Original file " + root.bytes(modelData.sourceFileBytes) : "")
                                    + (modelData.conversionPeakBytes ? "\nConversion peak (tracked + reserved): " + root.bytes(modelData.conversionPeakBytes) : "")
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
                                        ? "Cursor preparation reserve " + root.bytes(modelData.pendingPlaybackBudgetBytes) : "")]
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
            }
            footer: Text {
                width: assetList.width
                visible: assetList.count === 1
                height: visible ? implicitHeight + 16 : 0
                text: "No media loaded"
                horizontalAlignment: Text.AlignHCenter
                color: root.secondaryText
            }
        }
    }
}

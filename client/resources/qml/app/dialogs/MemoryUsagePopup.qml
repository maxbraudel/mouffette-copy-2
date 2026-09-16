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
    readonly property var usage: MediaMemory.summary
    readonly property real total: Math.max(1, usage.totalBytes || 1)
    readonly property color secondaryText: Theme.mutedText

    function bytes(value) {
        var gib = Number(value || 0) / 1073741824
        return gib >= 1 ? gib.toFixed(2) + " GiB"
                        : (Number(value || 0) / 1048576).toFixed(1) + " MiB"
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
        Text {
            Layout.fillWidth: true
            text: root.bytes(root.usage.mediaBytes) + " retained in media · "
                  + root.bytes(root.usage.totalBytes) + " total RAM"
            color: root.secondaryText
            font.pixelSize: 13
        }
        Text {
            Layout.fillWidth: true
            objectName: "memoryLoadableBudget"
            text: root.bytes(root.usage.loadableBytes) + " available for new media · "
                  + root.bytes(root.usage.reserveBytes) + " kept available for the system"
            color: Theme.text
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            text: root.bytes(root.usage.reservedBytes) + " pending media preparation · "
                  + root.bytes(root.usage.pendingPlaybackBudgetBytes) + " pending player preparation\n"
                  + root.bytes(root.usage.playbackBudgetBytes) + " total playback estimate"
            color: root.secondaryText
            font.pixelSize: 11
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
            font.pixelSize: 12
            wrapMode: Text.Wrap
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
        RowLayout {
            Layout.fillWidth: true
            spacing: 18
            Repeater {
                model: [
                    {label: "Mouffette", amount: root.usage.processBytes, tint: Theme.chartProcess},
                    {label: "System and other apps", amount: root.usage.otherBytes, tint: Theme.chartOther},
                    {label: "Available" + (root.usage.availableEstimated ? " (estimated)" : ""),
                     amount: root.usage.availableBytes, tint: Theme.chartAvailable}
                ]
                delegate: RowLayout {
                    required property var modelData
                    spacing: 5
                    Rectangle { width: 8; height: 8; radius: 2; color: modelData.tint }
                    Text {
                        text: modelData.label + "  " + root.bytes(modelData.amount)
                        color: Theme.text
                        font.pixelSize: 11
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            text: "Media from all open canvases and remote sessions"
            color: Theme.text
            font.pixelSize: 13
            font.bold: true
        }
        ListView {
            id: assetList
            objectName: "memoryAssetList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: MediaMemory.assets
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: assetList.width
                height: details.implicitHeight + 18
                color: Theme.surfaceBackground
                radius: 5
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 9
                    spacing: 10
                    ColumnLayout {
                        id: details
                        Layout.fillWidth: true
                        spacing: 3
                        Text {
                            Layout.fillWidth: true
                            text: modelData.displayName || "Media"
                            color: Theme.text
                            font.pixelSize: 13
                            elide: Text.ElideMiddle
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.stateLabel(modelData) + " · "
                                  + root.bytes(modelData.residentBytes) + " retained"
                                  + (modelData.state !== "ready" && modelData.estimatedBytes > 0
                                     ? " · " + root.bytes(modelData.estimatedBytes) + " to retain"
                                       + " · " + root.bytes(modelData.preparationBudgetBytes) + " preparation budget" : "")
                                  + (modelData.playbackBudgetBytes > 0
                                     ? " · " + root.bytes(modelData.playbackBudgetBytes) + " playback budget (estimated)" : "")
                                  + (modelData.occurrences > 1 ? " · " + modelData.occurrences + " uses" : "")
                                  + (modelData.protected ? " · In scene" : "")
                            color: root.secondaryText
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: modelData.error || ""
                            color: Theme.errorText
                            font.pixelSize: 11
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
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
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
            Text {
                anchors.centerIn: parent
                visible: assetList.count === 0
                text: "No media loaded"
                color: root.secondaryText
            }
        }
    }
}

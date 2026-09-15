import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mouffette.App
import "components"
import "dialogs"
import "pages"

QtObject {
    id: root
    required property var controller

    property BootstrapWindow bootstrap: BootstrapWindow {
        controller: root.controller
    }

    property ApplicationWindow window: ApplicationWindow {
        id: window
        objectName: "mainWindow"
        width: 600
        height: 500
        minimumWidth: 480
        visibility: root.controller.ready ? Window.Maximized : Window.Hidden
        title: "Mouffette"
        color: Theme.windowBackground

        onClosing: function(close) {
            close.accepted = false
            root.controller.hideWindow()
        }
        onVisibilityChanged: function() {
            root.controller.setWindowVisible(window.visible
                                             && window.visibility !== Window.Minimized)
        }

        HoverHandler {
            acceptedDevices: PointerDevice.Mouse
            onHoveredChanged: root.controller.setPointerInside(hovered)
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: Theme.windowMargin
            anchors.rightMargin: Theme.windowMargin
            anchors.bottomMargin: Theme.windowMargin
            anchors.leftMargin: Theme.windowMargin
            spacing: Theme.innerGap

            RowLayout {
                id: topBar
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.titleHeight
                spacing: 8

                Text {
                    text: root.controller.pageTitle
                    color: Theme.text
                    font.pixelSize: Theme.titleFontSize
                    font.bold: true
                    Layout.preferredHeight: Theme.titleHeight
                    verticalAlignment: Text.AlignVCenter
                }
                AppButton {
                    visible: root.controller.applicationPage !== 0
                    text: "← Go Back"
                    onClicked: root.controller.goBack()
                }
                SegmentedStatusCard {
                    visible: root.controller.applicationPage === 1 && window.width >= 600
                    primaryText: root.controller.remoteDisplayName
                    statusText: root.controller.remoteStatusText
                    statusKind: root.controller.remoteConnectionState
                    auxiliaryText: root.controller.remoteVolumeText
                    auxiliaryVisible: root.controller.remoteVolumeVisible
                    busy: root.controller.remoteBusy
                }
                AppButton {
                    visible: root.controller.applicationPage === 1 && root.controller.hasProject
                    text: "Delete project..."
                    enabled: root.controller.canDeleteProject
                    destructive: true
                    unavailableReason: "No project exists for this client"
                    onClicked: root.controller.requestDeleteProject()
                }
                Item { Layout.fillWidth: true }
                SegmentedStatusCard {
                    visible: root.controller.applicationPage !== 1 || window.width >= 1100
                    primaryText: "You"
                    statusText: root.controller.localStatusText
                    statusKind: root.controller.localConnectionState
                }
                AppButton {
                    visible: root.controller.applicationPage !== 1 || window.width >= 1100
                    text: root.controller.connectionEnabled ? "Disable" : "Enable"
                    onClicked: root.controller.toggleConnection()
                }
                AppButton {
                    visible: root.controller.applicationPage !== 1 || window.width >= 1100
                    text: "History"
                    onClicked: root.controller.showHistory()
                }
                AppButton {
                    visible: root.controller.applicationPage !== 1 || window.width >= 1100
                    text: "Settings"
                    onClicked: settings.open()
                }
            }

            SegmentedStatusCard {
                Layout.alignment: Qt.AlignLeft
                visible: root.controller.applicationPage === 1 && window.width < 600
                primaryText: root.controller.remoteDisplayName
                statusText: root.controller.remoteStatusText
                statusKind: root.controller.remoteConnectionState
                auxiliaryText: root.controller.remoteVolumeText
                auxiliaryVisible: root.controller.remoteVolumeVisible
                busy: root.controller.remoteBusy
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.controller.applicationPage

                ClientsPage { controller: root.controller }
                CanvasPage { controller: root.controller }
                HistoryPage { controller: root.controller }
            }
        }

        SettingsDialog {
            id: settings
            controller: root.controller
        }
        AppDialog {
            id: confirmation
            parent: Overlay.overlay
            anchors.centerIn: parent
            message: root.controller.dialogTitle
            detail: root.controller.dialogMessage
            acceptText: root.controller.dialogAcceptText
            rejectText: root.controller.dialogRejectText
            destructive: root.controller.dialogDestructive
            showReject: root.controller.dialogShowReject
            onAccepted: root.controller.acceptDialog()
            onRejected: root.controller.rejectDialog()
        }
        Connections {
            target: root.controller
            function onDialogRequested() { confirmation.open() }
            function onRaiseRequested() {
                window.show()
                window.raise()
                window.requestActivate()
            }
            function onHideRequested() { window.hide() }
        }
        ToastStack {
            controller: root.controller
        }
    }
}

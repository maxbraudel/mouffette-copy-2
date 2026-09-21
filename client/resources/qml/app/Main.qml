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
        visible: false
        title: "Mouffette"
        color: Theme.windowBackground
        palette: Theme.controlPalette

        WindowPresentation {
            id: presentation
            window: window
            alwaysOnTop: root.controller.settingsAppAlwaysOnTop
        }
        Component.onCompleted: {
            if (root.controller.ready) presentation.open()
        }

        Overlay.modal: Rectangle { color: Theme.modalScrim }

        onClosing: function(close) {
            close.accepted = false
            root.controller.hideWindow()
        }
        onVisibilityChanged: function() {
            root.controller.setWindowVisible(window.visible
                                             && window.visibility !== Window.Minimized)
        }

        HoverHandler {
            objectName: "windowActivityHover"
            // Cocoa reclassifies trackpads and Magic Mouse as TouchPad after
            // their first precise scroll. Both still move the same cursor.
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onHoveredChanged: root.controller.setPointerInside(hovered)
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: Theme.windowMargin
            anchors.rightMargin: Theme.windowMargin
            anchors.bottomMargin: Theme.windowMargin
            anchors.leftMargin: Theme.windowMargin
            spacing: Theme.innerGap

            Item {
                id: topBar
                objectName: "topBar"
                Layout.fillWidth: true
                implicitHeight: stacked ? Theme.controlHeight * (remoteStatus.visible ? 3 : 2)
                                          + gap * (remoteStatus.visible ? 2 : 1)
                                        : Theme.controlHeight

                readonly property real gap: 8
                // Measure the full labels, independently of the current layout,
                // so resizing never oscillates between text and icon widths.
                readonly property real navigationWidth: backButton.visible ? backButton.textWidth + gap : 0
                readonly property real firstRowWidth: pageTitle.implicitWidth + gap
                    + navigationWidth + actions.textWidth
                readonly property real singleRowWidth: firstRowWidth + localStatus.implicitWidth + gap
                    + (remoteStatus.visible ? remoteStatus.implicitWidth + gap : 0)
                readonly property bool stacked: width < singleRowWidth
                readonly property bool compactButtons: width < firstRowWidth

                Text {
                    id: pageTitle
                    objectName: "pageTitle"
                    x: backButton.visible ? backButton.width + topBar.gap : 0
                    width: topBar.stacked
                        ? Math.max(0, actions.x - x - topBar.gap)
                        : implicitWidth
                    height: Theme.titleHeight
                    text: root.controller.pageTitle
                    color: Theme.text
                    font.pixelSize: Theme.titleFontSize
                    font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                AppButton {
                    id: backButton
                    objectName: "backButton"
                    visible: root.controller.applicationPage !== 0
                    anchors.left: parent.left
                    text: "Go Back"
                    iconSource: "qrc:/icons/icons/arrow-left.svg"
                    iconOnly: topBar.compactButtons
                    onClicked: root.controller.goBack()
                }
                SegmentedStatusCard {
                    id: localStatus
                    objectName: "localConnectionStatus"
                    x: topBar.stacked ? 0 : actions.x - width - topBar.gap
                    y: topBar.stacked ? Theme.controlHeight + topBar.gap : 0
                    width: topBar.stacked ? topBar.width : implicitWidth
                    primaryText: "You"
                    profilePictureVisible: true
                    profilePictureSource: root.controller.settingsProfilePictureSource
                    statusText: root.controller.localStatusText
                    detailProvider: () => root.controller.localConnectionDetail()
                    statusKind: root.controller.localConnectionState
                }
                SegmentedStatusCard {
                    id: remoteStatus
                    objectName: "remoteConnectionStatus"
                    visible: root.controller.applicationPage === 1
                    x: topBar.stacked ? 0 : pageTitle.x + pageTitle.width + topBar.gap
                    y: topBar.stacked ? (Theme.controlHeight + topBar.gap) * 2 : 0
                    width: topBar.stacked ? topBar.width : implicitWidth
                    primaryText: root.controller.remoteDisplayName
                    profilePictureVisible: true
                    profileController: root.controller
                    profileEndpointId: root.controller.remoteEndpointId
                    statusText: root.controller.remoteStatusText
                    detailProvider: () => root.controller.remoteConnectionDetail()
                    statusKind: root.controller.remoteConnectionState
                    auxiliaryText: root.controller.remoteVolumeText
                    auxiliaryVisible: root.controller.remoteVolumeVisible
                    screenStatusVisible: true
                    screenContentEnabled: root.controller.screenContentVisible
                    screenAvailable: root.controller.remoteScreenAvailable
                    screenLoading: root.controller.remoteScreenLoading
                    busy: root.controller.remoteBusy
                }
                Row {
                    id: actions
                    x: topBar.width - width
                    spacing: topBar.gap
                    readonly property real textWidth: connectionButton.textWidth
                        + screenContentButton.textWidth + historyButton.textWidth
                        + settingsButton.textWidth + memoryButton.textWidth
                        + spacing * 4 + (deleteButton.visible ? deleteButton.textWidth + spacing : 0)

                    AppButton {
                        id: connectionButton
                        objectName: "connectionButton"
                        text: root.controller.connectionEnabled ? "Disable" : "Enable"
                        textVariants: ["Disable", "Enable"]
                        iconSource: "qrc:/icons/icons/power.svg"
                        iconOnly: topBar.compactButtons
                        onClicked: root.controller.setConnectionEnabled(!root.controller.connectionEnabled)
                    }
                    AppButton {
                        id: screenContentButton
                        objectName: "screenContentButton"
                        text: root.controller.screenContentVisible ? "Hide screen content" : "Show screen content"
                        textVariants: ["Hide screen content", "Show screen content"]
                        iconSource: root.controller.screenContentVisible
                            ? "qrc:/icons/icons/visibility-off.svg" : "qrc:/icons/icons/visibility-on.svg"
                        iconOnly: topBar.compactButtons
                        checked: root.controller.screenContentVisible
                        enabled: root.controller.ready && !root.controller.clearingStorage
                        onClicked: root.controller.setScreenContentVisible(!root.controller.screenContentVisible)
                    }
                    AppButton {
                        id: historyButton
                        objectName: "historyButton"
                        text: "History"
                        iconSource: "qrc:/icons/icons/history.svg"
                        iconOnly: topBar.compactButtons
                        onClicked: root.controller.showHistory()
                    }
                    AppButton {
                        id: settingsButton
                        objectName: "settingsButton"
                        text: "Settings"
                        iconSource: "qrc:/icons/icons/settings.svg"
                        iconOnly: topBar.compactButtons
                        onClicked: settings.open()
                    }
                    AppButton {
                        id: memoryButton
                        objectName: "memoryUsageButton"
                        text: "Usage RAM"
                        iconSource: "qrc:/icons/icons/memory.svg"
                        iconOnly: topBar.compactButtons
                        checked: memoryPopup.opened
                        onClicked: memoryPopup.open()
                    }
                    AppButton {
                        id: deleteButton
                        objectName: "deleteProjectButton"
                        visible: root.controller.applicationPage === 1 && root.controller.hasProject
                        text: "Delete project"
                        iconSource: "qrc:/icons/icons/delete.svg"
                        iconOnly: topBar.compactButtons
                        enabled: root.controller.canDeleteProject
                        destructive: true
                        unavailableReason: "No project exists for this client"
                        onClicked: root.controller.requestDeleteProject()
                    }
                }
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
        MemoryUsagePopup { id: memoryPopup }
        AppDialog {
            id: confirmation
            controller: root.controller
            peers: root.controller.dialogPeers
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
            function onReadyChanged() {
                if (root.controller.ready) presentation.open()
            }
            function onDialogRequested() { confirmation.open() }
            function onRaiseRequested() {
                if (!root.controller.ready) {
                    root.bootstrap.show()
                    root.bootstrap.raise()
                    root.bootstrap.requestActivate()
                    return
                }
                presentation.open()
            }
            function onHideRequested() { window.hide() }
        }
        ToastStack {
            controller: root.controller
        }
    }
}

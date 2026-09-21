import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Mouffette.App

Item {
    id: root

    enum StatusKind {
        Connected,
        Warning,
        Error
    }

    property var detailProvider: null
    property string currentDetail: ""
    HoverHandler {
        id: detailHover
        onHoveredChanged: if (hovered && root.detailProvider) root.currentDetail = root.detailProvider()
    }
    ToolTip.visible: detailHover.hovered && currentDetail.length > 0
    ToolTip.text: currentDetail
    ToolTip.delay: 400
    Timer {
        interval: 500
        running: detailHover.hovered && root.detailProvider !== null
        repeat: true
        onTriggered: root.currentDetail = root.detailProvider()
    }

    property string primaryText: ""
    property string statusText: "DISCONNECTED"
    property int statusKind: SegmentedStatusCard.Error
    property string auxiliaryText: ""
    property bool auxiliaryVisible: auxiliaryText.length > 0
    property bool screenStatusVisible: false
    property bool screenContentEnabled: true
    property bool screenAvailable: false
    property bool screenLoading: false
    readonly property string screenStatusText: !screenContentEnabled ? "Screen disabled"
        : screenLoading ? "Screen loading" : screenAvailable ? "Screen available" : "Screen not available"
    readonly property int screenStatusKind: !screenContentEnabled ? SegmentedStatusCard.Error
        : screenLoading ? SegmentedStatusCard.Warning
        : screenAvailable ? SegmentedStatusCard.Connected : SegmentedStatusCard.Error
    readonly property color screenStatusColor: foregroundForStatus(screenStatusKind)
    readonly property color screenStatusBackground: backgroundForStatus(screenStatusKind)
    property bool profilePictureVisible: false
    property url profilePictureSource: ""
    property var profileController: null
    property string profileEndpointId: ""
    // Leave room above and below the avatar so the card border cannot cover it.
    readonly property real profilePictureSize: Math.max(0, height - 4)
    readonly property real profilePictureWidth: profilePictureVisible ? profilePictureSize + 8 : 0
    readonly property real primaryWidth: Math.max(20, primaryLabel.implicitWidth
                                                 + profilePictureWidth + Theme.segmentPadding * 2)
    readonly property real statusWidth: statusMetrics.maximumWidth + Theme.segmentPadding * 2
    readonly property real auxiliaryWidth: auxiliaryVisible
        ? Math.max(40, auxiliaryMetrics.maximumWidth + Theme.segmentPadding * 2 + 16 + 4) : 0
    readonly property real screenStatusWidth: screenStatusVisible
        ? screenStatusMetrics.maximumWidth + Theme.segmentPadding * 2 + 16 + 4 : 0
    readonly property bool availableStatus: statusText.trim().toUpperCase() === "AVAILABLE"

    function foregroundForStatus(kind) {
        return kind === SegmentedStatusCard.Connected ? Theme.connectedText
            : kind === SegmentedStatusCard.Warning ? Theme.warningText : Theme.errorText
    }
    function backgroundForStatus(kind) {
        return kind === SegmentedStatusCard.Connected ? Theme.connectedBackground
            : kind === SegmentedStatusCard.Warning ? Theme.warningBackground : Theme.errorBackground
    }
    readonly property color statusForeground: availableStatus ? Theme.availableText : foregroundForStatus(statusKind)
    readonly property color statusBackground: availableStatus ? Theme.buttonBackground : backgroundForStatus(statusKind)

    ConnectionStatusMetrics {
        id: statusMetrics
        text: root.statusText
        uppercase: true
        font: statusLabel.font
    }

    StateTextMetrics {
        id: auxiliaryMetrics
        text: root.auxiliaryText
        textVariants: Array.from({length: 101}, function(_, value) { return value + "%" })
        font: auxiliaryLabel.font
    }

    StateTextMetrics {
        id: screenStatusMetrics
        text: root.screenStatusText
        textVariants: ["Screen available", "Screen not available", "Screen disabled", "Screen loading"]
        font: screenStatusLabel.font
    }

    implicitWidth: primaryWidth + 1 + statusWidth
                   + (auxiliaryVisible ? 1 + auxiliaryWidth : 0)
                   + (screenStatusVisible ? 1 + screenStatusWidth : 0)
    implicitHeight: Theme.controlHeight
    height: Theme.controlHeight

    Row {
        id: segments
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: statusSegment
            objectName: "statusSegment"
            height: root.height
            width: root.statusWidth
            color: root.statusBackground
            topLeftRadius: Theme.controlRadius
            bottomLeftRadius: Theme.controlRadius

            Text {
                id: statusLabel
                objectName: "connectionStatusLabel"
                anchors.fill: parent
                anchors.leftMargin: Theme.segmentPadding
                anchors.rightMargin: Theme.segmentPadding
                text: root.statusText
                color: root.statusForeground
                font.pixelSize: Theme.controlFontSize
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }

        Rectangle {
            width: 1
            height: root.height
            color: Theme.border
        }

        Rectangle {
            id: primarySegment
            clip: true
            height: root.height
            width: Math.max(0, root.width - 1 - root.statusWidth
                           - (root.auxiliaryVisible ? 1 + root.auxiliaryWidth : 0)
                           - (root.screenStatusVisible ? 1 + root.screenStatusWidth : 0))
            color: "transparent"

            ProfilePicture {
                id: profilePicture
                objectName: "statusProfilePicture"
                visible: root.profilePictureVisible
                anchors.left: parent.left
                anchors.leftMargin: Theme.segmentPadding
                anchors.verticalCenter: parent.verticalCenter
                width: root.profilePictureSize
                height: width
                source: root.profilePictureSource
                controller: root.profileController
                endpointId: root.profileEndpointId
            }

            Text {
                id: primaryLabel
                anchors.fill: parent
                anchors.leftMargin: Theme.segmentPadding + root.profilePictureWidth
                anchors.rightMargin: Theme.segmentPadding
                text: root.primaryText
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.titleFontSize
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
            }
        }

        Rectangle {
            visible: root.screenStatusVisible
            width: visible ? 1 : 0
            height: root.height
            color: Theme.border
        }

        Rectangle {
            id: screenStatusSegment
            objectName: "screenAvailabilitySegment"
            visible: root.screenStatusVisible
            height: root.height
            width: root.screenStatusWidth
            color: root.screenStatusBackground
            topRightRadius: root.auxiliaryVisible ? 0 : Theme.controlRadius
            bottomRightRadius: topRightRadius
            Accessible.role: Accessible.StaticText
            Accessible.name: root.screenStatusText

            Image {
                id: screenStatusIcon
                objectName: "screenAvailabilityIcon"
                anchors.left: parent.left
                anchors.leftMargin: Theme.segmentPadding
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                source: root.screenContentEnabled && (root.screenAvailable || root.screenLoading)
                    ? "qrc:/icons/icons/screen.svg" : "qrc:/icons/icons/screen-off.svg"
                sourceSize: Qt.size(width * 4, height * 4)
                fillMode: Image.PreserveAspectFit
                layer.enabled: true
                layer.effect: MultiEffect {
                    contrast: -1
                    brightness: 0.5
                    colorization: 1
                    colorizationColor: root.screenStatusColor
                }
            }

            Text {
                id: screenStatusLabel
                objectName: "screenAvailabilityLabel"
                anchors.left: screenStatusIcon.right
                anchors.leftMargin: 4
                anchors.right: parent.right
                anchors.rightMargin: Theme.segmentPadding
                height: parent.height
                text: root.screenStatusText
                color: root.screenStatusColor
                font: statusLabel.font
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Rectangle {
            visible: root.auxiliaryVisible
            width: visible ? 1 : 0
            height: root.height
            color: Theme.border
        }

        Rectangle {
            id: auxiliarySegment
            objectName: "volumeSegment"
            visible: root.auxiliaryVisible
            height: root.height
            width: root.auxiliaryWidth
            color: "transparent"

            Image {
                id: volumeIcon
                anchors.left: parent.left
                anchors.leftMargin: Theme.segmentPadding
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                source: "qrc:/icons/icons/volume-on.svg"
                sourceSize: Qt.size(width * 4, height * 4)
                fillMode: Image.PreserveAspectFit
                layer.enabled: true
                layer.effect: MultiEffect {
                    contrast: -1
                    brightness: 0.5
                    colorization: 1
                    colorizationColor: Theme.text
                }
            }

            Text {
                id: auxiliaryLabel
                anchors.left: volumeIcon.right
                anchors.leftMargin: 4
                anchors.right: parent.right
                anchors.rightMargin: Theme.segmentPadding
                height: parent.height
                text: root.auxiliaryText
                color: Theme.text
                font.pixelSize: Theme.titleFontSize
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
            }
        }

    }

    // Painted last. Segment fills can never erase or shorten this border.
    Rectangle {
        objectName: "cardBorder"
        anchors.fill: parent
        z: 100
        color: "transparent"
        radius: Theme.controlRadius
        border.width: 1
        border.color: Theme.border
    }

}

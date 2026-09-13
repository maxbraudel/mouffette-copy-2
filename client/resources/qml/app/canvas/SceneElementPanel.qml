import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mouffette.App
import "../components"

Rectangle {
    id: root

    required property var session
    readonly property var settings: session ? session.mediaSettings : null

    visible: !!session && session.settingsVisible
    width: 350
    height: Math.min(parent ? parent.height - 72 : 620,
                     content.implicitHeight + 2)
    radius: Theme.overlayRadius
    color: Theme.overlayBackground
    border.width: 1
    border.color: Theme.overlayBorder
    clip: true

    component SectionTitle: Text {
        Layout.fillWidth: true
        Layout.topMargin: 8
        color: Theme.overlayText
        font.pixelSize: 12
        font.bold: true
    }

    component ToggleRow: RowLayout {
        required property string label
        required property bool value
        property bool fieldVisible: false
        property bool fieldEnabled: true
        property string fieldText: ""
        property string suffix: ""
        signal valueEdited(bool checked)
        signal textEdited(string text)

        Layout.fillWidth: true
        spacing: 6

        AppCheckBox {
            Layout.fillWidth: true
            text: parent.label
            checked: parent.value
            onToggled: parent.valueEdited(checked)
        }
        AppTextField {
            visible: parent.fieldVisible
            enabled: parent.fieldEnabled
            Layout.preferredWidth: 70
            text: parent.fieldText
            horizontalAlignment: TextInput.AlignRight
            onEditingFinished: parent.textEdited(text)
        }
        Text {
            visible: parent.fieldVisible && parent.suffix.length > 0
            text: parent.suffix
            color: Theme.overlayText
            opacity: 0.75
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: content
            width: flick.width
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.topMargin: 10
                Text {
                    Layout.fillWidth: true
                    text: root.settings && root.settings.available
                          ? root.settings.mediaName : "Element settings"
                    color: Theme.overlayText
                    font.bold: true
                    elide: Text.ElideRight
                }
                AppButton {
                    text: "×"
                    onClicked: root.session.settingsVisible = false
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.margins: 18
                visible: !root.settings || !root.settings.available
                text: "Select an element to edit its scene settings."
                color: Theme.overlayText
                opacity: 0.75
                wrapMode: Text.Wrap
            }

            ColumnLayout {
                visible: root.settings && root.settings.available
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.bottomMargin: 12
                spacing: 5

                SectionTitle { text: "Display" }
                ToggleRow {
                    label: "Display automatically"
                    value: root.settings.displayAutomatically
                    onValueEdited: checked => root.settings.displayAutomatically = checked
                }
                ToggleRow {
                    label: "Display delay"
                    value: root.settings.displayDelayEnabled
                    fieldVisible: true
                    fieldEnabled: value
                    fieldText: root.settings.displayDelayText
                    suffix: "s"
                    onValueEdited: checked => root.settings.displayDelayEnabled = checked
                    onTextEdited: text => root.settings.displayDelayText = text
                }
                ToggleRow {
                    label: "Fade in"
                    value: root.settings.fadeInEnabled
                    fieldVisible: true
                    fieldEnabled: value
                    fieldText: root.settings.fadeInText
                    suffix: "s"
                    onValueEdited: checked => root.settings.fadeInEnabled = checked
                    onTextEdited: text => root.settings.fadeInText = text
                }
                ToggleRow {
                    label: "Fade out"
                    value: root.settings.fadeOutEnabled
                    fieldVisible: true
                    fieldEnabled: value
                    fieldText: root.settings.fadeOutText
                    suffix: "s"
                    onValueEdited: checked => root.settings.fadeOutEnabled = checked
                    onTextEdited: text => root.settings.fadeOutText = text
                }
                ToggleRow {
                    label: "Opacity override"
                    value: root.settings.opacityOverrideEnabled
                    fieldVisible: true
                    fieldEnabled: value
                    fieldText: root.settings.opacityText
                    suffix: "%"
                    onValueEdited: checked => root.settings.opacityOverrideEnabled = checked
                    onTextEdited: text => root.settings.opacityText = text
                }
                ToggleRow {
                    label: "Hide delay"
                    value: root.settings.hideDelayEnabled
                    fieldVisible: true
                    fieldEnabled: value
                    fieldText: root.settings.hideDelayText
                    suffix: "s"
                    onValueEdited: checked => root.settings.hideDelayEnabled = checked
                    onTextEdited: text => root.settings.hideDelayText = text
                }

                ColumnLayout {
                    visible: root.settings.video
                    Layout.fillWidth: true
                    spacing: 5

                    SectionTitle { text: "Playback" }
                    ToggleRow {
                        label: "Play automatically"
                        value: root.settings.playAutomatically
                        onValueEdited: checked => root.settings.playAutomatically = checked
                    }
                    ToggleRow {
                        label: "Play delay"
                        value: root.settings.playDelayEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.playDelayText
                        suffix: "s"
                        onValueEdited: checked => root.settings.playDelayEnabled = checked
                        onTextEdited: text => root.settings.playDelayText = text
                    }
                    ToggleRow {
                        label: "Pause delay"
                        value: root.settings.pauseDelayEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.pauseDelayText
                        suffix: "s"
                        onValueEdited: checked => root.settings.pauseDelayEnabled = checked
                        onTextEdited: text => root.settings.pauseDelayText = text
                    }
                    ToggleRow {
                        label: "Repeat"
                        value: root.settings.repeatEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.repeatCountText
                        suffix: "×"
                        onValueEdited: checked => root.settings.repeatEnabled = checked
                        onTextEdited: text => root.settings.repeatCountText = text
                    }
                    ToggleRow {
                        label: "Hide when video ends"
                        value: root.settings.hideWhenVideoEnds
                        onValueEdited: checked => root.settings.hideWhenVideoEnds = checked
                    }

                    SectionTitle { text: "Audio" }
                    ToggleRow {
                        label: "Volume override"
                        value: root.settings.volumeOverrideEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.volumeText
                        suffix: "%"
                        onValueEdited: checked => root.settings.volumeOverrideEnabled = checked
                        onTextEdited: text => root.settings.volumeText = text
                    }
                    ToggleRow {
                        label: "Unmute automatically"
                        value: root.settings.unmuteAutomatically
                        onValueEdited: checked => root.settings.unmuteAutomatically = checked
                    }
                    ToggleRow {
                        label: "Unmute delay"
                        value: root.settings.unmuteDelayEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.unmuteDelayText
                        suffix: "s"
                        onValueEdited: checked => root.settings.unmuteDelayEnabled = checked
                        onTextEdited: text => root.settings.unmuteDelayText = text
                    }
                    ToggleRow {
                        label: "Mute delay"
                        value: root.settings.muteDelayEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.muteDelayText
                        suffix: "s"
                        onValueEdited: checked => root.settings.muteDelayEnabled = checked
                        onTextEdited: text => root.settings.muteDelayText = text
                    }
                    ToggleRow {
                        label: "Mute when video ends"
                        value: root.settings.muteWhenVideoEnds
                        onValueEdited: checked => root.settings.muteWhenVideoEnds = checked
                    }
                    ToggleRow {
                        label: "Audio fade in"
                        value: root.settings.audioFadeInEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.audioFadeInText
                        suffix: "s"
                        onValueEdited: checked => root.settings.audioFadeInEnabled = checked
                        onTextEdited: text => root.settings.audioFadeInText = text
                    }
                    ToggleRow {
                        label: "Audio fade out"
                        value: root.settings.audioFadeOutEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.audioFadeOutText
                        suffix: "s"
                        onValueEdited: checked => root.settings.audioFadeOutEnabled = checked
                        onTextEdited: text => root.settings.audioFadeOutText = text
                    }
                }

                ColumnLayout {
                    visible: root.settings.textMedia
                    Layout.fillWidth: true
                    spacing: 5

                    SectionTitle { text: "Text" }
                    ToggleRow {
                        label: "Text color"
                        value: root.settings.textColorOverrideEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.textColor
                        onValueEdited: checked => root.settings.textColorOverrideEnabled = checked
                        onTextEdited: text => root.settings.textColor = text
                    }
                    ToggleRow {
                        label: "Highlight"
                        value: root.settings.highlightEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.highlightColor
                        onValueEdited: checked => root.settings.highlightEnabled = checked
                        onTextEdited: text => root.settings.highlightColor = text
                    }
                    ToggleRow {
                        label: "Border width"
                        value: root.settings.textBorderWidthOverrideEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.textBorderWidthText
                        suffix: "%"
                        onValueEdited: checked => root.settings.textBorderWidthOverrideEnabled = checked
                        onTextEdited: text => root.settings.textBorderWidthText = text
                    }
                    ToggleRow {
                        label: "Border color"
                        value: root.settings.textBorderColorOverrideEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.textBorderColor
                        onValueEdited: checked => root.settings.textBorderColorOverrideEnabled = checked
                        onTextEdited: text => root.settings.textBorderColor = text
                    }
                    ToggleRow {
                        label: "Font weight"
                        value: root.settings.fontWeightOverrideEnabled
                        fieldVisible: true
                        fieldEnabled: value
                        fieldText: root.settings.fontWeightText
                        onValueEdited: checked => root.settings.fontWeightOverrideEnabled = checked
                        onTextEdited: text => root.settings.fontWeightText = text
                    }
                    ToggleRow {
                        label: "Italic"
                        value: root.settings.italic
                        onValueEdited: checked => root.settings.italic = checked
                    }
                    ToggleRow {
                        label: "Underline"
                        value: root.settings.underline
                        onValueEdited: checked => root.settings.underline = checked
                    }
                    ToggleRow {
                        label: "Uppercase"
                        value: root.settings.uppercase
                        onValueEdited: checked => root.settings.uppercase = checked
                    }
                }
            }
        }
    }
}

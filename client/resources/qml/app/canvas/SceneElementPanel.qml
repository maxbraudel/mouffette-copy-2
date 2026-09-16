import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Mouffette.App
import Mouffette.Canvas
import "../components"

Rectangle {
    id: root

    required property var session
    property real maximumHeight: 620
    property int activeTab: 0
    readonly property var settings: session ? session.mediaSettings : null
    readonly property string selectedMediaId: settings && settings.available
                                               ? settings.mediaId : ""
    property int editGeneration: 0
    readonly property real activePageHeight: activeTab === 0
                                                   ? scenePage.height
                                                   : elementPage.height
    readonly property real desiredHeight: 42 + activePageHeight

    objectName: "sceneElementPanel"
    visible: !!session && session.settingsVisible
             && !!settings && settings.available
    width: 221
    height: visible ? Math.max(1, Math.min(maximumHeight, desiredHeight)) : 0
    radius: Theme.overlayRadius
    color: Theme.overlayBackground
    border.width: 1
    border.color: Theme.overlayBorder
    clip: true

    onActiveTabChanged: contentFlick.contentY = 0
    onVisibleChanged: if (!visible) contentFlick.contentY = 0
    onSelectedMediaIdChanged: {
        editGeneration += 1
        contentFlick.contentY = 0
    }

    // Keep blank areas of the floating overlay from passing clicks through to
    // the canvas. Controls declared below still receive their events first.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onPressed: root.forceActiveFocus()
    }

    component PanelTab: AbstractButton {
        id: tab

        required property bool active

        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.PageTab
        Accessible.name: text

        contentItem: Text {
            text: tab.text
            color: Theme.overlayText
            font.pixelSize: 14
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            color: {
                if (tab.down)
                    return Qt.rgba(1, 1, 1, 0.15)
                if (tab.active)
                    return tab.hovered ? Qt.rgba(1, 1, 1, 0.15)
                                       : Qt.rgba(1, 1, 1, 0.10)
                return tab.hovered ? Qt.rgba(1, 1, 1, 0.05) : "transparent"
            }
        }
    }

    component RangeButton: AbstractButton {
        id: button
        implicitHeight: 28
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Button
        Accessible.name: text
        contentItem: Text {
            text: button.text
            color: button.enabled ? Theme.overlayText : "#808080"
            font.pixelSize: 12
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 4
            color: button.down ? "#345780" : button.hovered ? "#36404c" : "transparent"
            border.color: button.enabled ? Theme.overlayBorder : "#505050"
        }
    }

    component SettingsCheckBox: AppCheckBox {
        implicitHeight: 25
        textColor: enabled ? Theme.overlayText : "#808080"
        uncheckedBorderColor: enabled ? "#c8c8c8" : "#808080"
        checkedColor: Theme.brandBlue
        checkmarkColor: "white"
    }

    component OptionRow: Item {
        id: option

        property string label: ""
        property bool checkedValue: false
        property bool rowEnabled: true
        property bool valueVisible: false
        property string valueText: ""
        property string suffix: ""
        property string inputKind: "decimal"
        property real valueWidth: 38
        property string checkObjectName: ""
        property string fieldObjectName: ""
        signal checkedEdited(bool checked)
        signal textEdited(string text)

        width: parent ? parent.width : 191
        height: 25

        function normalizedValue(candidate) {
            var value = String(candidate).trim().replace(",", ".")
            if (value.length === 0)
                return valueText

            if (inputKind === "repeat") {
                if (value === "∞" || value.toLowerCase() === "i")
                    return "∞"
                if (!/^\d+$/.test(value))
                    return valueText
                value = value.replace(/^0+/, "")
                if (value.length === 0)
                    return valueText
                return value.length > 5 ? "∞" : value
            }

            if (inputKind === "percent") {
                if (!/^\d+$/.test(value))
                    return valueText
                return String(Math.min(100, Number(value)))
            }

            if (inputKind === "borderPercent") {
                if (!/^\d{1,3}(?:\.\d+)?$/.test(value))
                    return valueText
                return String(Math.min(100, Number(value)))
            }

            if (inputKind === "weight") {
                if (!/^\d+$/.test(value))
                    return valueText
                return String(Math.max(1, Math.min(900, Number(value))))
            }

            var expression = inputKind === "signedDecimal"
                    ? /^-?\d{1,5}(?:\.\d+)?$/
                    : /^\d{1,5}(?:\.\d+)?$/
            return expression.test(value) && isFinite(Number(value))
                    ? value : valueText
        }

        SettingsCheckBox {
            id: checkBox
            objectName: option.checkObjectName
            anchors.left: parent.left
            anchors.top: parent.top
            width: valueGroup.visible
                   ? Math.max(0, parent.width - valueGroup.width)
                   : parent.width
            height: parent.height
            text: option.label
            checked: option.checkedValue
            enabled: option.rowEnabled
            onToggled: option.checkedEdited(checked)
        }

        Row {
            id: valueGroup
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: option.valueVisible
            spacing: 2

            FocusScope {
                id: valueField

                property bool committing: false
                property bool editing: false
                property bool clearOnFirstType: false
                property bool inputEnabled: option.rowEnabled
                property string draftText: option.valueText

                objectName: option.fieldObjectName
                width: option.valueWidth
                height: 22
                activeFocusOnTab: inputEnabled
                Accessible.role: Accessible.SpinBox
                Accessible.name: option.label
                Accessible.description: draftText

                function commitValue() {
                    if (committing)
                        return
                    committing = true
                    var normalized = option.normalizedValue(draftText)
                    draftText = normalized
                    if (normalized !== option.valueText)
                        option.textEdited(normalized)
                    committing = false
                }

                function cancelEdit() {
                    draftText = option.valueText
                    editing = false
                    focus = false
                }

                function activate() {
                    if (!inputEnabled)
                        return
                    if (editing)
                        commitValue()
                    draftText = option.valueText
                    clearOnFirstType = true
                    editing = true
                    forceActiveFocus()
                }

                function baseForCharacter() {
                    if (clearOnFirstType || draftText === "..." || draftText === "∞") {
                        clearOnFirstType = false
                        return ""
                    }
                    return draftText
                }

                function appendDigit(character) {
                    if (option.inputKind === "repeat" && character === "0")
                        return

                    var candidate = baseForCharacter() + character
                    var digitCount = candidate.replace(/\D/g, "").length
                    if (option.inputKind === "repeat") {
                        draftText = digitCount > 5 ? "∞" : candidate
                        return
                    }
                    if (option.inputKind === "percent") {
                        draftText = Number(candidate) > 100 ? "100" : candidate
                        return
                    }
                    if (option.inputKind === "weight") {
                        draftText = Number(candidate) > 900 ? "900" : candidate
                        return
                    }
                    if (option.inputKind === "borderPercent") {
                        draftText = Number(candidate) > 100 ? "100" : candidate
                        return
                    }
                    if (digitCount <= 5)
                        draftText = candidate
                }

                onActiveFocusChanged: {
                    if (!activeFocus && editing) {
                        commitValue()
                        editing = false
                    }
                }

                onInputEnabledChanged: {
                    if (!inputEnabled && editing)
                        cancelEdit()
                }

                Keys.onPressed: function(event) {
                    if (!valueField.inputEnabled)
                        return

                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        commitValue()
                        editing = false
                        focus = false
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Escape) {
                        cancelEdit()
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Backspace) {
                        draftText = "..."
                        clearOnFirstType = true
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_I && option.inputKind === "repeat") {
                        draftText = "∞"
                        clearOnFirstType = false
                        event.accepted = true
                        return
                    }

                    var character = event.text
                    if (character === ",")
                        character = "."
                    if (character === ".") {
                        var decimalKind = option.inputKind === "decimal"
                                || option.inputKind === "signedDecimal"
                                || option.inputKind === "borderPercent"
                        var decimalBase = baseForCharacter()
                        if (decimalKind && decimalBase.length > 0
                                && decimalBase !== "-"
                                && decimalBase.indexOf(".") < 0)
                            draftText = decimalBase + "."
                        event.accepted = true
                        return
                    }
                    if (character === "-" && option.inputKind === "signedDecimal") {
                        var signedBase = baseForCharacter()
                        if (signedBase.indexOf("-") < 0)
                            draftText = "-" + signedBase
                        event.accepted = true
                        return
                    }
                    if (/^\d$/.test(character)) {
                        appendDigit(character)
                        event.accepted = true
                        return
                    }
                    event.accepted = true
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: valueField.activeFocus ? Theme.brandBlue : "#3c3c3c"
                    border.width: 1
                    border.color: valueField.activeFocus
                                  ? Theme.brandBlue : "#c8c8c8"
                    opacity: valueField.inputEnabled ? 1.0 : 0.55

                    Text {
                        anchors.fill: parent
                        leftPadding: 4
                        rightPadding: 4
                        text: valueField.draftText
                        color: "white"
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.AllButtons
                    preventStealing: true
                    propagateComposedEvents: false
                    cursorShape: Qt.ArrowCursor

                    onPressed: function(mouse) {
                        mouse.accepted = true
                        if (mouse.button === Qt.LeftButton)
                            valueField.activate()
                    }
                    onReleased: function(mouse) {
                        mouse.accepted = true
                    }
                    onClicked: function(mouse) {
                        mouse.accepted = true
                    }
                    onDoubleClicked: function(mouse) {
                        mouse.accepted = true
                    }
                }

                Connections {
                    target: option
                    function onValueTextChanged() {
                        if (!valueField.editing && !valueField.committing)
                            valueField.draftText = option.valueText
                    }
                }
                Connections {
                    target: root
                    function onEditGenerationChanged() {
                        valueField.cancelEdit()
                    }
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: option.suffix
                color: option.rowEnabled ? Theme.overlayText : "#808080"
                font.pixelSize: 14
                font.weight: Font.Medium
            }
        }
    }

    component ColorOptionRow: Item {
        id: colorOption

        property string label: ""
        property bool checkedValue: false
        property string colorValue: "#ffffffff"
        property string dialogTitle: "Select Color"
        property string checkObjectName: ""
        property string swatchObjectName: ""
        signal checkedEdited(bool checked)
        signal colorEdited(string color)

        width: parent ? parent.width : 191
        height: 25

        SettingsCheckBox {
            objectName: colorOption.checkObjectName
            anchors.left: parent.left
            anchors.top: parent.top
            width: parent.width - swatch.width
            height: parent.height
            text: colorOption.label
            checked: colorOption.checkedValue
            onToggled: colorOption.checkedEdited(checked)
        }

        AbstractButton {
            id: swatch
            objectName: colorOption.swatchObjectName
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            height: 22
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: colorOption.dialogTitle
            onClicked: {
                colorDialog.selectedColor = colorOption.colorValue
                colorDialog.open()
            }

            background: Rectangle {
                radius: 6
                color: colorOption.colorValue
                border.width: 1
                border.color: swatch.activeFocus || swatch.down
                              ? Theme.brandBlue : "#c8c8c8"
            }
        }

        ColorDialog {
            id: colorDialog
            title: colorOption.dialogTitle
            options: ColorDialog.ShowAlphaChannel
            onAccepted: colorOption.colorEdited(selectedColor.toString())
        }
    }

    component SettingsSection: Column {
        id: section

        property string title: ""
        property bool firstSection: false
        default property alias rows: rowsColumn.data

        width: parent ? parent.width : 191
        spacing: 5

        Item {
            visible: !section.firstSection
            width: 1
            height: 10
        }

        Text {
            width: parent.width
            text: section.title
            color: Theme.overlayText
            font.pixelSize: 14
            font.bold: true
        }

        Item {
            width: 1
            height: 10
        }

        Column {
            id: rowsColumn
            width: parent.width
            spacing: 5
        }
    }

    Item {
        id: tabStrip
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        height: 39

        PanelTab {
            id: sceneTab
            objectName: "sceneSettingsTab"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: (parent.width - tabDivider.width) / 2
            text: "Scene"
            active: root.activeTab === 0
            onClicked: root.activeTab = 0
        }

        Rectangle {
            id: tabDivider
            anchors.left: sceneTab.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: Theme.overlayBorder
        }

        PanelTab {
            objectName: "elementSettingsTab"
            anchors.left: tabDivider.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            text: "Element"
            active: root.activeTab === 1
            onClicked: root.activeTab = 1
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 1
        anchors.rightMargin: 1
        y: 40
        height: 1
        color: Theme.overlayBorder
    }

    Flickable {
        id: contentFlick
        objectName: "settingsContentFlick"
        x: 1
        y: 41
        width: Math.max(0, root.width - 2)
        height: Math.max(0, root.height - 42)
        contentWidth: width
        contentHeight: root.activePageHeight
        boundsBehavior: Flickable.StopAtBounds
        readonly property bool overflowing: contentHeight > height + 0.5
        interactive: overflowing
        clip: true
        onMovementStarted: settingsScrollBar.reveal()

        ScrollBar.vertical: ScrollBar {
            id: settingsScrollBar

            property bool recentlyActive: false

            function reveal() {
                recentlyActive = true
                hideTimer.restart()
            }

            objectName: "settingsOverlayScrollBar"
            policy: ScrollBar.AsNeeded
            width: 8
            visible: contentFlick.overflowing
            opacity: recentlyActive || hovered || pressed ? 1.0 : 0.0
            onPressedChanged: if (pressed) reveal()
            contentItem: Rectangle {
                implicitWidth: 8
                implicitHeight: 24
                radius: 4
                color: parent.pressed ? Qt.rgba(1, 1, 1, 0.70)
                                      : parent.hovered ? Qt.rgba(1, 1, 1, 0.55)
                                                       : Qt.rgba(1, 1, 1, 0.35)
            }
            background: Item {}

            Timer {
                id: hideTimer
                interval: UiTiming.scrollbarHideDelayMs
                onTriggered: settingsScrollBar.recentlyActive = false
            }
        }

        Item {
            id: scenePage
            objectName: "sceneSettingsPage"
            width: contentFlick.width
            height: sceneContent.height + 20
            visible: root.activeTab === 0

            Column {
                id: sceneContent
                x: 15
                y: 10
                width: parent.width - 30
                spacing: 5

                SettingsSection {
                    objectName: "sceneImageSection"
                    title: "Image"
                    firstSection: true

                    OptionRow {
                        label: "Display automatically"
                        checkObjectName: "displayAutomaticallyCheck"
                        checkedValue: !!root.settings
                                      && root.settings.displayAutomatically
                        onCheckedEdited: checked => {
                            root.settings.displayAutomatically = checked
                            if (!checked)
                                root.settings.displayDelayEnabled = false
                        }
                    }
                    OptionRow {
                        label: "Display delay: "
                        checkObjectName: "displayDelayCheck"
                        fieldObjectName: "displayDelayField"
                        checkedValue: !!root.settings
                                      && root.settings.displayDelayEnabled
                        rowEnabled: !!root.settings
                                    && root.settings.displayAutomatically
                        valueVisible: true
                        valueText: root.settings
                                   ? root.settings.displayDelayText : "1"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.displayDelayEnabled = checked
                        onTextEdited: text => root.settings.displayDelayText = text
                    }
                    OptionRow {
                        label: "Hide delay: "
                        checkObjectName: "hideDelayCheck"
                        fieldObjectName: "hideDelayField"
                        checkedValue: !!root.settings && root.settings.hideDelayEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.hideDelayText : "1"
                        suffix: "s"
                        inputKind: "signedDecimal"
                        onCheckedEdited: checked => root.settings.hideDelayEnabled = checked
                        onTextEdited: text => root.settings.hideDelayText = text
                    }
                    OptionRow {
                        visible: !!root.settings && root.settings.video
                        label: "Hide when video ends"
                        checkObjectName: "hideWhenVideoEndsCheck"
                        checkedValue: !!root.settings
                                      && root.settings.hideWhenVideoEnds
                        onCheckedEdited: checked => root.settings.hideWhenVideoEnds = checked
                    }
                }

                SettingsSection {
                    objectName: "sceneAudioSection"
                    visible: !!root.settings && root.settings.video
                    title: "Audio"

                    OptionRow {
                        label: "Unmute automatically"
                        checkObjectName: "unmuteAutomaticallyCheck"
                        checkedValue: !!root.settings
                                      && root.settings.unmuteAutomatically
                        onCheckedEdited: checked => {
                            root.settings.unmuteAutomatically = checked
                            if (!checked)
                                root.settings.unmuteDelayEnabled = false
                        }
                    }
                    OptionRow {
                        label: "Unmute delay: "
                        checkObjectName: "unmuteDelayCheck"
                        fieldObjectName: "unmuteDelayField"
                        checkedValue: !!root.settings
                                      && root.settings.unmuteDelayEnabled
                        rowEnabled: !!root.settings
                                    && root.settings.unmuteAutomatically
                        valueVisible: true
                        valueText: root.settings
                                   ? root.settings.unmuteDelayText : "0"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.unmuteDelayEnabled = checked
                        onTextEdited: text => root.settings.unmuteDelayText = text
                    }
                    OptionRow {
                        label: "Mute delay: "
                        checkObjectName: "muteDelayCheck"
                        fieldObjectName: "muteDelayField"
                        checkedValue: !!root.settings && root.settings.muteDelayEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.muteDelayText : "1"
                        suffix: "s"
                        inputKind: "signedDecimal"
                        onCheckedEdited: checked => root.settings.muteDelayEnabled = checked
                        onTextEdited: text => root.settings.muteDelayText = text
                    }
                    OptionRow {
                        label: "Mute when video ends"
                        checkObjectName: "muteWhenVideoEndsCheck"
                        checkedValue: !!root.settings
                                      && root.settings.muteWhenVideoEnds
                        onCheckedEdited: checked => root.settings.muteWhenVideoEnds = checked
                    }
                }

                SettingsSection {
                    objectName: "sceneVideoSection"
                    visible: !!root.settings && root.settings.video
                    title: "Video"

                    OptionRow {
                        label: "Play automatically"
                        checkObjectName: "playAutomaticallyCheck"
                        checkedValue: !!root.settings
                                      && root.settings.playAutomatically
                        onCheckedEdited: checked => {
                            root.settings.playAutomatically = checked
                            if (!checked)
                                root.settings.playDelayEnabled = false
                        }
                    }
                    OptionRow {
                        label: "Play delay: "
                        checkObjectName: "playDelayCheck"
                        fieldObjectName: "playDelayField"
                        checkedValue: !!root.settings && root.settings.playDelayEnabled
                        rowEnabled: !!root.settings
                                    && root.settings.playAutomatically
                        valueVisible: true
                        valueText: root.settings ? root.settings.playDelayText : "1"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.playDelayEnabled = checked
                        onTextEdited: text => root.settings.playDelayText = text
                    }
                    OptionRow {
                        label: "Pause delay: "
                        checkObjectName: "pauseDelayCheck"
                        fieldObjectName: "pauseDelayField"
                        checkedValue: !!root.settings && root.settings.pauseDelayEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.pauseDelayText : "1"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.pauseDelayEnabled = checked
                        onTextEdited: text => root.settings.pauseDelayText = text
                    }
                    OptionRow {
                        label: "Repeat "
                        checkObjectName: "repeatCheck"
                        fieldObjectName: "repeatField"
                        checkedValue: !!root.settings && root.settings.repeatEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.repeatCountText : "1"
                        suffix: " times"
                        inputKind: "repeat"
                        onCheckedEdited: checked => root.settings.repeatEnabled = checked
                        onTextEdited: text => root.settings.repeatCountText = text
                    }
                }
            }
        }

        Item {
            id: elementPage
            objectName: "elementSettingsPage"
            width: contentFlick.width
            height: elementContent.height + 20
            visible: root.activeTab === 1

            Column {
                id: elementContent
                x: 15
                y: 10
                width: parent.width - 30
                spacing: 5

                SettingsSection {
                    objectName: "elementImageSection"
                    title: "Image"
                    firstSection: true

                    OptionRow {
                        label: "Image fade in: "
                        checkObjectName: "imageFadeInCheck"
                        fieldObjectName: "imageFadeInField"
                        checkedValue: !!root.settings && root.settings.fadeInEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.fadeInText : "1"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.fadeInEnabled = checked
                        onTextEdited: text => root.settings.fadeInText = text
                    }
                    OptionRow {
                        label: "Image fade out: "
                        checkObjectName: "imageFadeOutCheck"
                        fieldObjectName: "imageFadeOutField"
                        checkedValue: !!root.settings && root.settings.fadeOutEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.fadeOutText : "1"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.fadeOutEnabled = checked
                        onTextEdited: text => root.settings.fadeOutText = text
                    }
                    OptionRow {
                        label: "Opacity: "
                        checkObjectName: "opacityCheck"
                        fieldObjectName: "opacityField"
                        checkedValue: !!root.settings
                                      && root.settings.opacityOverrideEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.opacityText : "100"
                        suffix: "%"
                        inputKind: "percent"
                        onCheckedEdited: checked => root.settings.opacityOverrideEnabled = checked
                        onTextEdited: text => root.settings.opacityText = text
                    }
                }

                SettingsSection {
                    objectName: "elementVideoRangeSection"
                    visible: !!root.settings && root.settings.video
                    title: "Video"

                    Row {
                        width: parent.width
                        spacing: 6

                        RangeButton {
                            objectName: "videoStartButton"
                            width: (parent.width - parent.spacing) / 2
                            text: root.settings && root.settings.hasVideoStart
                                  ? "Remove start" : "Place start"
                            enabled: !!root.settings && root.settings.available
                            onClicked: root.settings.toggleVideoStart()
                        }
                        RangeButton {
                            objectName: "videoEndButton"
                            width: (parent.width - parent.spacing) / 2
                            text: root.settings && root.settings.hasVideoEnd
                                  ? "Remove end" : "Place end"
                            enabled: !!root.settings && root.settings.available
                            onClicked: root.settings.toggleVideoEnd()
                        }
                    }
                }

                SettingsSection {
                    objectName: "elementAudioSection"
                    visible: !!root.settings && root.settings.video
                    title: "Audio"

                    OptionRow {
                        label: "Volume: "
                        checkObjectName: "volumeCheck"
                        fieldObjectName: "volumeField"
                        checkedValue: !!root.settings
                                      && root.settings.audioEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.volumeText : "100"
                        suffix: "%"
                        inputKind: "percent"
                        onCheckedEdited: checked => root.settings.audioEnabled = checked
                        onTextEdited: text => root.settings.volumeText = text
                    }
                    OptionRow {
                        label: "Audio fade in: "
                        checkObjectName: "audioFadeInCheck"
                        fieldObjectName: "audioFadeInField"
                        checkedValue: !!root.settings
                                      && root.settings.audioFadeInEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.audioFadeInText : "1"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.audioFadeInEnabled = checked
                        onTextEdited: text => root.settings.audioFadeInText = text
                    }
                    OptionRow {
                        label: "Audio fade out: "
                        checkObjectName: "audioFadeOutCheck"
                        fieldObjectName: "audioFadeOutField"
                        checkedValue: !!root.settings
                                      && root.settings.audioFadeOutEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.audioFadeOutText : "1"
                        suffix: "s"
                        onCheckedEdited: checked => root.settings.audioFadeOutEnabled = checked
                        onTextEdited: text => root.settings.audioFadeOutText = text
                    }
                }

                SettingsSection {
                    objectName: "textSettingsSection"
                    visible: !!root.settings && root.settings.textMedia
                    title: "Text"

                    ColorOptionRow {
                        label: "Text color: "
                        dialogTitle: "Select Text Color"
                        checkObjectName: "textColorCheck"
                        swatchObjectName: "textColorSwatch"
                        checkedValue: !!root.settings
                                      && root.settings.textColorOverrideEnabled
                        colorValue: root.settings
                                    ? root.settings.textColor : "#ffffffff"
                        onCheckedEdited: checked => root.settings.textColorOverrideEnabled = checked
                        onColorEdited: color => root.settings.textColor = color
                    }
                    ColorOptionRow {
                        label: "Highlight: "
                        dialogTitle: "Select Highlight Color"
                        checkObjectName: "highlightCheck"
                        swatchObjectName: "highlightSwatch"
                        checkedValue: !!root.settings && root.settings.highlightEnabled
                        colorValue: root.settings
                                    ? root.settings.highlightColor : "#00000000"
                        onCheckedEdited: checked => root.settings.highlightEnabled = checked
                        onColorEdited: color => root.settings.highlightColor = color
                    }
                    OptionRow {
                        label: "Border width: "
                        checkObjectName: "textBorderWidthCheck"
                        fieldObjectName: "textBorderWidthField"
                        checkedValue: !!root.settings
                                      && root.settings.textBorderWidthOverrideEnabled
                        valueVisible: true
                        valueText: root.settings
                                   ? root.settings.textBorderWidthText : "0"
                        suffix: "%"
                        inputKind: "borderPercent"
                        onCheckedEdited: checked => root.settings.textBorderWidthOverrideEnabled = checked
                        onTextEdited: text => root.settings.textBorderWidthText = text
                    }
                    ColorOptionRow {
                        label: "Border color: "
                        dialogTitle: "Select Border Color"
                        checkObjectName: "textBorderColorCheck"
                        swatchObjectName: "textBorderColorSwatch"
                        checkedValue: !!root.settings
                                      && root.settings.textBorderColorOverrideEnabled
                        colorValue: root.settings
                                    ? root.settings.textBorderColor : "#00000000"
                        onCheckedEdited: checked => root.settings.textBorderColorOverrideEnabled = checked
                        onColorEdited: color => root.settings.textBorderColor = color
                    }
                    OptionRow {
                        label: "Font weight: "
                        checkObjectName: "fontWeightCheck"
                        fieldObjectName: "fontWeightField"
                        checkedValue: !!root.settings
                                      && root.settings.fontWeightOverrideEnabled
                        valueVisible: true
                        valueText: root.settings ? root.settings.fontWeightText : "400"
                        inputKind: "weight"
                        onCheckedEdited: checked => root.settings.fontWeightOverrideEnabled = checked
                        onTextEdited: text => root.settings.fontWeightText = text
                    }
                    OptionRow {
                        label: "Underline"
                        checkObjectName: "underlineCheck"
                        checkedValue: !!root.settings && root.settings.underline
                        onCheckedEdited: checked => root.settings.underline = checked
                    }
                    OptionRow {
                        label: "Italic"
                        checkObjectName: "italicCheck"
                        checkedValue: !!root.settings && root.settings.italic
                        onCheckedEdited: checked => root.settings.italic = checked
                    }
                    OptionRow {
                        label: "Uppercase"
                        checkObjectName: "uppercaseCheck"
                        checkedValue: !!root.settings && root.settings.uppercase
                        onCheckedEdited: checked => root.settings.uppercase = checked
                    }
                }
            }
        }
    }
}

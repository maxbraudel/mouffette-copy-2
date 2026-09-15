pragma Singleton

import QtQuick
import Mouffette.Canvas

QtObject {
    id: theme

    readonly property SystemPalette activePalette: SystemPalette {
        colorGroup: SystemPalette.Active
    }
    readonly property SystemPalette disabledPalette: SystemPalette {
        colorGroup: SystemPalette.Disabled
    }

    function mix(left, right, leftRatio) {
        var ratio = Math.max(0.0, Math.min(1.0, leftRatio))
        return Qt.rgba(left.r * ratio + right.r * (1.0 - ratio),
                       left.g * ratio + right.g * (1.0 - ratio),
                       left.b * ratio + right.b * (1.0 - ratio),
                       left.a * ratio + right.a * (1.0 - ratio))
    }

    readonly property color windowBackground: activePalette.base
    readonly property color text: activePalette.text
    readonly property color mutedText: activePalette.mid
    readonly property color disabledText: disabledPalette.text
    readonly property color border: mix(activePalette.text, activePalette.base, 0.2)
    readonly property color interactionBackground: Qt.rgba(activePalette.text.r,
                                                            activePalette.text.g,
                                                            activePalette.text.b,
                                                            8 / 255)

    readonly property color brandBlue: "#4a90e2"
    readonly property color brandBlueLight: Qt.rgba(74 / 255, 144 / 255, 226 / 255, 38 / 255)
    readonly property color brandBlueDark: "#1f4ea8"

    readonly property color connectedText: "#4c9b50"
    readonly property color connectedBackground: Qt.rgba(76 / 255, 175 / 255, 80 / 255, 38 / 255)
    readonly property color availableText: Qt.rgba(activePalette.text.r,
                                                   activePalette.text.g,
                                                   activePalette.text.b,
                                                   0.55)
    readonly property color warningText: "#ffa000"
    readonly property color warningBackground: Qt.rgba(255 / 255, 152 / 255, 0, 38 / 255)
    readonly property color errorText: "#ff5753"
    readonly property color errorBackground: Qt.rgba(244 / 255, 67 / 255, 54 / 255, 38 / 255)

    readonly property color buttonBackground: Qt.rgba(0.5, 0.5, 0.5, 20 / 255)
    readonly property color buttonHover: Qt.rgba(0.5, 0.5, 0.5, 41 / 255)
    readonly property color buttonPressed: Qt.rgba(0.5, 0.5, 0.5, 61 / 255)
    readonly property color buttonDisabled: Qt.rgba(0.5, 0.5, 0.5, 15 / 255)
    readonly property color primaryBackground: brandBlueLight
    readonly property color primaryHover: Qt.rgba(74 / 255, 144 / 255, 226 / 255, 56 / 255)
    readonly property color primaryPressed: Qt.rgba(74 / 255, 144 / 255, 226 / 255, 77 / 255)

    readonly property color overlayBackground: Qt.rgba(50 / 255, 50 / 255, 50 / 255, 240 / 255)
    readonly property color overlayActiveBackground: Qt.rgba(52 / 255, 87 / 255, 128 / 255, 240 / 255)
    readonly property color overlayText: Qt.rgba(1, 1, 1, 230 / 255)
    readonly property color overlayBorder: "#646464"
    readonly property color mediaUploaded: "#2ecc71"
    readonly property color mediaNotUploaded: "#f39c12"
    readonly property color mediaProgress: "#2d8cff"
    readonly property color mediaProgressBackground: Qt.rgba(1, 1, 1, 38 / 255)
    readonly property color overlaySecondaryText: Qt.rgba(1, 1, 1, 217 / 255)
    readonly property color overlayDisabledText: Qt.rgba(1, 1, 1, 0.4)
    readonly property color overlayDisabledBackground: Qt.rgba(1, 1, 1, 0.04)
    readonly property color overlaySceneText: "#ff96ff"
    readonly property color overlaySceneBackground: Qt.rgba(1, 0, 1, 38 / 255)
    readonly property color overlaySceneHover: Qt.rgba(1, 0, 1, 56 / 255)
    readonly property color overlayScenePressed: Qt.rgba(1, 0, 1, 77 / 255)
    readonly property color overlayUploadedHover: Qt.rgba(76 / 255, 175 / 255, 80 / 255, 56 / 255)
    readonly property color overlayUploadedPressed: Qt.rgba(76 / 255, 175 / 255, 80 / 255, 77 / 255)

    readonly property int windowMargin: 20
    readonly property int innerGap: 20
    readonly property int controlHeight: 24
    readonly property int controlRadius: 6
    readonly property int controlMinWidth: 80
    readonly property int controlFontSize: 13
    readonly property int titleFontSize: 16
    readonly property int titleHeight: 24
    readonly property int segmentPadding: 6
    readonly property int overlayRadius: 8
    readonly property int overlayButtonHeight: 40
    readonly property int toastMarginLeft: 40
    readonly property int toastMarginBottom: 40
    readonly property int toastSpacing: 10
    readonly property int toastRadius: 8
    readonly property int toastAnimationDuration: UiTiming.toastAnimationDurationMs
    readonly property int toastSlideDistance: 20
    readonly property int toastTextSize: 13
}

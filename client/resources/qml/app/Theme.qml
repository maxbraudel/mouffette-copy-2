pragma Singleton

import QtQuick
import Mouffette.Canvas

QtObject {
    id: theme

    // One source of truth for application chrome. SystemPalette updates these
    // bindings immediately when the OS appearance/application palette changes.
    // Authored media/text colors and projected scene output are not UI chrome.
    readonly property SystemPalette activePalette: SystemPalette {
        colorGroup: SystemPalette.Active
    }
    readonly property bool dark: (0.2126 * windowBackground.r
                                 + 0.7152 * windowBackground.g
                                 + 0.0722 * windowBackground.b) < 0.5

    function mix(left, right, leftRatio) {
        var ratio = Math.max(0.0, Math.min(1.0, leftRatio))
        return Qt.rgba(left.r * ratio + right.r * (1.0 - ratio),
                       left.g * ratio + right.g * (1.0 - ratio),
                       left.b * ratio + right.b * (1.0 - ratio),
                       left.a * ratio + right.a * (1.0 - ratio))
    }

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, Math.max(0, Math.min(1, alpha)))
    }

    // Neutral surfaces are opaque so floating controls never inherit the
    // unpredictable colors of the media underneath them.
    // Some native palettes expose translucent text. Resolve neutral roles to
    // opaque RGB before mixing so surface opacity never depends on that alpha.
    readonly property color windowBackground: withAlpha(activePalette.base, 1)
    readonly property color text: withAlpha(activePalette.text, 1)
    readonly property color shadowPigment: "#000000"
    readonly property color surfaceBackground: mix(text, windowBackground, dark ? 0.035 : 0.025)
    readonly property color elevatedBackground: mix(text, windowBackground, dark ? 0.065 : 0.0)
    readonly property color recessedBackground: mix(dark ? shadowPigment : text, windowBackground, dark ? 0.16 : 0.055)
    // Palette.mid is a bevel/border role, not a readable secondary foreground.
    readonly property color mutedText: mix(text, windowBackground, 0.70)
    readonly property color disabledText: mix(text, windowBackground, 0.42)
    readonly property color border: mix(text, windowBackground, 0.20)
    readonly property color controlBorder: mix(text, windowBackground, 0.48)
    readonly property color controlDisabledBorder: border
    readonly property color interactionBackground: withAlpha(text, 0.035)

    // Pigments are used only for translucent tints. Foregrounds have separate
    // light/dark values so blue, green, amber and red labels remain readable.
    readonly property color bluePigment: "#4a90e2"
    readonly property color greenPigment: "#4caf50"
    readonly property color amberPigment: "#ff9800"
    readonly property color redPigment: "#f44336"
    readonly property color scenePigment: "#b258b6"
    readonly property color accent: dark ? "#8bbcff" : "#245ca6"
    readonly property color onAccent: dark ? "#17202b" : "#ffffff"
    readonly property color focusBorder: accent
    readonly property color brandBlue: accent
    readonly property color brandBlueLight: withAlpha(bluePigment, 38 / 255)
    readonly property color brandBlueDark: accent
    readonly property color connectedText: dark ? "#88d498" : "#216536"
    readonly property color connectedBackground: withAlpha(greenPigment, 38 / 255)
    readonly property color availableText: mutedText
    readonly property color warningText: dark ? "#f3c26b" : "#885000"
    readonly property color warningBackground: withAlpha(amberPigment, 38 / 255)
    readonly property color errorText: dark ? "#ff9b96" : "#a52529"
    readonly property color errorBackground: withAlpha(redPigment, 38 / 255)
    readonly property color sceneText: dark ? "#deb0f0" : "#853580"
    readonly property color sceneBackground: withAlpha(scenePigment, 38 / 255)

    // Controls: neutral interactions plus tinted accent/destructive variants.
    readonly property color buttonBackground: withAlpha(text, 0.055)
    readonly property color buttonHover: withAlpha(text, 0.09)
    readonly property color buttonPressed: withAlpha(text, 0.13)
    readonly property color buttonDisabled: withAlpha(text, 0.03)
    readonly property color primaryBackground: brandBlueLight
    readonly property color primaryHover: withAlpha(bluePigment, 0.20)
    readonly property color primaryPressed: withAlpha(bluePigment, 0.25)
    readonly property color destructiveBackground: errorBackground
    readonly property color destructiveHover: withAlpha(redPigment, 0.20)
    readonly property color destructivePressed: withAlpha(redPigment, 0.25)
    readonly property color fieldBackground: recessedBackground
    readonly property color fieldBorder: controlBorder
    readonly property color selectionBackground: accent
    readonly property color selectionText: onAccent
    readonly property color scrollbar: mix(text, elevatedBackground, 0.38)
    readonly property color scrollbarHover: mix(text, elevatedBackground, 0.55)
    readonly property color scrollbarPressed: mix(text, elevatedBackground, 0.72)

    // Canvas chrome uses the same neutral hierarchy as the surrounding app.
    readonly property color canvasBackground: recessedBackground
    readonly property color canvasScreenBackground: mix(text, canvasBackground, dark ? 0.16 : 0.18)
    readonly property color canvasPrimaryScreenBackground: mix(bluePigment, canvasScreenBackground, 0.55)
    readonly property color canvasScreenBorder: controlBorder
    readonly property color canvasScreenText: text
    readonly property color canvasLabelShadow: withAlpha(canvasScreenBackground, 0.75)
    readonly property color selectionBorder: accent
    readonly property color selectionFill: withAlpha(bluePigment, 0.15)
    readonly property color selectionHandle: elevatedBackground
    readonly property color snapGuide: accent
    readonly property color uiZoneFill: withAlpha(elevatedBackground, 0.70)
    readonly property color uiZoneSystemFill: withAlpha(elevatedBackground, 0.95)
    readonly property color mediaPlaceholder: mix(text, canvasBackground, 0.20)
    // The remote pointer must stay legible on arbitrary authored content.
    readonly property color remoteCursorFill: "#ffffff"
    readonly property color remoteCursorBorder: "#e6000000"

    readonly property color overlayBackground: elevatedBackground
    readonly property color overlayActiveBackground: mix(bluePigment, overlayBackground, 0.22)
    readonly property color overlayHover: mix(text, overlayBackground, 0.055)
    readonly property color overlayPressed: mix(text, overlayBackground, 0.10)
    readonly property color overlaySelected: mix(bluePigment, overlayBackground, 0.16)
    readonly property color overlayText: text
    readonly property color overlaySecondaryText: mutedText
    readonly property color overlayDisabledText: disabledText
    readonly property color overlayBorder: border
    readonly property color overlayDisabledBackground: overlayBackground
    readonly property color overlaySceneText: sceneText
    readonly property color overlaySceneBackground: sceneBackground
    readonly property color overlaySceneHover: withAlpha(scenePigment, 0.20)
    readonly property color overlayScenePressed: withAlpha(scenePigment, 0.25)
    readonly property color overlayUploadedHover: withAlpha(greenPigment, 0.20)
    readonly property color overlayUploadedPressed: withAlpha(greenPigment, 0.25)
    readonly property color mediaUploaded: connectedText
    readonly property color mediaNotUploaded: warningText
    readonly property color mediaProgress: accent
    readonly property color mediaProgressBackground: mix(text, overlayBackground, 0.12)
    readonly property color sliderTrack: mix(text, overlayBackground, 0.25)
    readonly property color sliderFill: accent
    readonly property color sliderHandle: accent
    readonly property color toolTipBackground: elevatedBackground
    readonly property color toolTipText: text
    readonly property color toolTipBorder: border
    readonly property color modalScrim: withAlpha(shadowPigment, dark ? 0.50 : 0.25)
    // ToastStack paints this opaque base, then a translucent severity tint.
    readonly property color toastBackground: windowBackground
    readonly property color chartProcess: accent
    readonly property color chartOther: mutedText
    readonly property color chartAvailable: connectedText

    // Native Qt Quick controls (menus, dialogs, scrollbars, selections) inherit
    // the same semantic colors as our custom components, including disabled UI.
    readonly property Palette controlPalette: Palette {
        window: theme.windowBackground
        windowText: theme.text
        base: theme.fieldBackground
        alternateBase: theme.surfaceBackground
        text: theme.text
        placeholderText: theme.mutedText
        button: theme.surfaceBackground
        buttonText: theme.text
        highlight: theme.selectionBackground
        highlightedText: theme.selectionText
        accent: theme.accent
        light: theme.elevatedBackground
        midlight: theme.surfaceBackground
        mid: theme.border
        dark: theme.controlBorder
        shadow: theme.modalScrim
        toolTipBase: theme.toolTipBackground
        toolTipText: theme.toolTipText
        link: theme.accent
        linkVisited: theme.sceneText
        disabled.text: theme.disabledText
        disabled.windowText: theme.disabledText
        disabled.buttonText: theme.disabledText
        disabled.highlight: theme.buttonDisabled
        disabled.highlightedText: theme.disabledText
    }

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

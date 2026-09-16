import QtQuick

// Reserve every state before the first transition. Measure with the actual
// font, so theme/font changes still participate in QML's implicit sizing.
FontMetrics {
    id: metrics
    property string text: ""
    property list<string> textVariants: []
    readonly property real maximumWidth: measure(text, textVariants, font)

    function measure(currentText, variants, measuredFont) {
        // measuredFont makes changes to the font a binding dependency.
        var widest = advanceWidth(currentText)
        for (var i = 0; i < variants.length; ++i)
            widest = Math.max(widest, advanceWidth(variants[i]))
        return Math.ceil(widest)
    }
}

import QtQuick 2.15

QtObject {
    id: arbiter

    property string mode: "idle"      // idle | move | resize | pan | text
    property string ownerId: ""

    function isIdle() {
        return mode === "idle"
    }

    function canStart(requestedMode, requestedOwnerId) {
        var owner = requestedOwnerId || ""
        return mode === "idle"
            || (mode === requestedMode && ownerId === owner)
    }

    function begin(requestedMode, requestedOwnerId) {
        var owner = requestedOwnerId || ""
        if (!canStart(requestedMode, owner))
            return false
        mode = requestedMode
        ownerId = owner
        return true
    }

    function end(expectedMode, expectedOwnerId) {
        var owner = expectedOwnerId || ""
        if (mode !== expectedMode)
            return
        if (ownerId !== "" && owner !== "" && ownerId !== owner)
            return
        mode = "idle"
        ownerId = ""
    }
}

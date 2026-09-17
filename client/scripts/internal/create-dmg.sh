#!/usr/bin/env bash

# Use the application logo for the mounted install volume as well as the app.
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "Usage: create-dmg.sh <app> <output.dmg> <volume-name> <icon.icns>" >&2
    exit 2
fi

APP_PATH="$1"
OUTPUT_DMG="$2"
VOLUME_NAME="$3"
ICON_PATH="$4"
[[ -d "$APP_PATH" && -f "$ICON_PATH" ]] || {
    echo "Application or volume icon is missing." >&2
    exit 1
}
command -v SetFile >/dev/null || {
    echo "SetFile is required to set the DMG volume icon (install Xcode Command Line Tools)." >&2
    exit 1
}

DMG_WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mouffette-dmg.XXXXXX")"
MOUNT_DIR="$DMG_WORK_DIR/mount"
MOUNTED=false
cleanup() {
    if [[ "$MOUNTED" == true ]]; then
        hdiutil detach "$MOUNT_DIR" -quiet || return
    fi
    rm -rf "$DMG_WORK_DIR"
}
trap cleanup EXIT

hdiutil create -volname "$VOLUME_NAME" -srcfolder "$APP_PATH" \
    -fs HFS+ -format UDRW "$DMG_WORK_DIR/writable.dmg"
mkdir "$MOUNT_DIR"
hdiutil attach "$DMG_WORK_DIR/writable.dmg" -nobrowse -noautoopen \
    -mountpoint "$MOUNT_DIR" -quiet
MOUNTED=true
cp "$ICON_PATH" "$MOUNT_DIR/.VolumeIcon.icns"
SetFile -a C "$MOUNT_DIR"
hdiutil detach "$MOUNT_DIR" -quiet
MOUNTED=false
hdiutil convert "$DMG_WORK_DIR/writable.dmg" -format UDZO -ov -o "$OUTPUT_DMG"

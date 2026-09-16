#!/usr/bin/env bash

set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_ROOT="$(cd "$SCRIPTS_DIR/.." && pwd)"
cd "$CLIENT_ROOT"

SKIP_TESTS=false
REQUIRE_SIGNING=false
NOTARIZE=true

usage() {
    cat <<'EOF'
Usage: ./scripts/package-release.sh [--skip-tests] [--require-signing] [--no-notarize]

Environment variables:
  MOUFFETTE_MACOS_SIGN_IDENTITY  Developer ID Application identity
  MOUFFETTE_NOTARY_PROFILE       notarytool keychain profile

Without an identity, a locally testable ad-hoc signed DMG is produced. Use
--require-signing for a public release so missing credentials fail the build.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-tests) SKIP_TESTS=true; shift ;;
        --require-signing) REQUIRE_SIGNING=true; shift ;;
        --no-notarize) NOTARIZE=false; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "package-release.sh creates the macOS package. Use package-release.ps1 on Windows." >&2
    exit 2
fi

if [[ -z "${MOUFFETTE_QT_ROOT:-}" ]] && command -v brew >/dev/null 2>&1; then
    MOUFFETTE_QT_ROOT="$(brew --prefix qt 2>/dev/null || true)"
fi
if [[ -z "${MOUFFETTE_QT_ROOT:-}" || ! -d "$MOUFFETTE_QT_ROOT" ]]; then
    echo "No Qt 6 installation selected. Set MOUFFETTE_QT_ROOT." >&2
    exit 1
fi
export MOUFFETTE_QT_ROOT

if [[ "$REQUIRE_SIGNING" == true && -z "${MOUFFETTE_MACOS_SIGN_IDENTITY:-}" ]]; then
    echo "MOUFFETTE_MACOS_SIGN_IDENTITY is required for a public release." >&2
    exit 1
fi
if [[ "$NOTARIZE" == true && -n "${MOUFFETTE_NOTARY_PROFILE:-}" \
    && -z "${MOUFFETTE_MACOS_SIGN_IDENTITY:-}" ]]; then
    echo "Notarization requires MOUFFETTE_MACOS_SIGN_IDENTITY." >&2
    exit 1
fi
if [[ "$REQUIRE_SIGNING" == true && "$NOTARIZE" == true \
    && -z "${MOUFFETTE_NOTARY_PROFILE:-}" ]]; then
    echo "MOUFFETTE_NOTARY_PROFILE is required for a public notarized release." >&2
    exit 1
fi

"$SCRIPTS_DIR/build-release.sh"

if [[ "$SKIP_TESTS" == false ]]; then
    echo "Running Release tests..."
    ctest --preset macos-release --parallel
fi

BUILD_DIR="$CLIENT_ROOT/out/build/macos-release"
FINAL_STAGE_DIR="$CLIENT_ROOT/out/stage/macos-release"
PACKAGE_DIR="$CLIENT_ROOT/out/packages"
TEMP_BASE="${TMPDIR:-/tmp}"
WORK_DIR="$(realpath "$(mktemp -d "${TEMP_BASE%/}/mouffette-package.XXXXXX")")"
STAGE_DIR="$WORK_DIR/stage"
APP="$STAGE_DIR/Mouffette.app"
DEPENDENCY_REPORT=""

cleanup() {
    if [[ -n "$DEPENDENCY_REPORT" && -f "$DEPENDENCY_REPORT" ]]; then
        rm -f "$DEPENDENCY_REPORT"
    fi
    if [[ "${MOUFFETTE_KEEP_PACKAGE_WORK_DIR:-0}" == "1" ]]; then
        echo "Keeping package work directory for diagnostics: $WORK_DIR"
    elif [[ -n "$WORK_DIR" && -d "$WORK_DIR" ]]; then
        cmake -E rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

case "$FINAL_STAGE_DIR" in
    "$CLIENT_ROOT/out/stage/"*) ;;
    *) echo "Refusing unsafe staging path: $FINAL_STAGE_DIR" >&2; exit 1 ;;
esac
cmake -E rm -rf "$FINAL_STAGE_DIR"
cmake -E make_directory "$STAGE_DIR" "$FINAL_STAGE_DIR" "$PACKAGE_DIR"

# macdeployqt miscomputes relative loader paths when the working path contains
# decomposed Unicode. Deploy under an ASCII temporary directory, then copy the
# validated relocatable bundle back into the repository staging directory.
echo "Installing Release application into an isolated staging tree..."
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR"
if [[ ! -x "$APP/Contents/MacOS/Mouffette" ]]; then
    echo "Installed application not found at $APP" >&2
    exit 1
fi

# Qt's generic `-qmldir` deployment copies every child of Homebrew's aggregate
# QtQuick tree (including unrelated Qt3D/VirtualKeyboard modules). Install the
# exact imports and runtime plugins used by Mouffette, then ask macdeployqt to
# rewrite/deploy the dependencies of those binaries.
MACDEPLOYQT="$MOUFFETTE_QT_ROOT/bin/macdeployqt"
if [[ ! -x "$MACDEPLOYQT" ]]; then
    echo "macdeployqt not found at $MACDEPLOYQT" >&2
    exit 1
fi

QT_QML_DIR="$MOUFFETTE_QT_ROOT/share/qt/qml"
QT_PLUGIN_DIR="$MOUFFETTE_QT_ROOT/share/qt/plugins"
APP_QML_DIR="$APP/Contents/Resources/qml"
APP_PLUGIN_DIR="$APP/Contents/PlugIns"

copy_qml_root_files() {
    local relative_dir="$1"
    local source_dir="$QT_QML_DIR/$relative_dir"
    local target_dir="$APP_QML_DIR/$relative_dir"
    [[ -d "$source_dir" ]] || { echo "Missing Qt QML module: $source_dir" >&2; exit 1; }
    cmake -E make_directory "$target_dir"
    # Homebrew exposes many QML files as relative symlinks into split formulae.
    # Dereference them so the final bundle never points back to the Cellar.
    find -L "$source_dir" -maxdepth 1 -type f -exec cp -Lp {} "$target_dir" \;
}

for QML_ROOT in QML QtMultimedia QtQml QtQuick QtQuick/Controls; do
    copy_qml_root_files "$QML_ROOT"
done
for QML_MODULE in \
    QtQml/Models \
    QtQml/WorkerScript \
    QtQuick/Controls/Basic \
    QtQuick/Controls/impl \
    QtQuick/Controls/macOS \
    QtQuick/Layouts \
    QtQuick/NativeStyle \
    QtQuick/Templates \
    QtQuick/Window; do
    [[ -d "$QT_QML_DIR/$QML_MODULE" ]] || {
        echo "Missing Qt QML module: $QT_QML_DIR/$QML_MODULE" >&2
        exit 1
    }
    cmake -E make_directory "$(dirname "$APP_QML_DIR/$QML_MODULE")"
    cp -RLp "$(realpath "$QT_QML_DIR/$QML_MODULE")/." "$APP_QML_DIR/$QML_MODULE/"
done

copy_qt_plugin() {
    local plugin_type="$1"
    local plugin_name="$2"
    local source_file="$QT_PLUGIN_DIR/$plugin_type/$plugin_name"
    [[ -f "$source_file" ]] || { echo "Missing Qt plugin: $source_file" >&2; exit 1; }
    cmake -E make_directory "$APP_PLUGIN_DIR/$plugin_type"
    cp -p "$source_file" "$APP_PLUGIN_DIR/$plugin_type/"
}

copy_qt_plugin platforms libqcocoa.dylib
copy_qt_plugin styles libqmacstyle.dylib
copy_qt_plugin tls libqsecuretransportbackend.dylib
copy_qt_plugin tls libqopensslbackend.dylib
copy_qt_plugin tls libqcertonlybackend.dylib
copy_qt_plugin iconengines libqsvgicon.dylib
# The build bundles our Qt Darwin plugin with the first-seek precision fix.
# Never overwrite it with the unpatched plugin from the Qt installation.
[[ -f "$APP_PLUGIN_DIR/multimedia/libdarwinmediaplugin.dylib" ]] || {
    echo "Missing patched Darwin media plugin in $APP" >&2
    exit 1
}
copy_qt_plugin networkinformation libqapplenetworkinformation.dylib
for IMAGE_PLUGIN in libqgif.dylib libqwebp.dylib libqico.dylib libqmacheif.dylib \
    libqjpeg.dylib libqtiff.dylib libqsvg.dylib libqicns.dylib; do
    copy_qt_plugin imageformats "$IMAGE_PLUGIN"
done

DEPLOY_ARGS=(
    "$APP"
    -no-plugins
    -always-overwrite
    -verbose=0
)

while IFS= read -r BUNDLED_BINARY; do
    if file "$BUNDLED_BINARY" | grep -q 'Mach-O'; then
        DEPLOY_ARGS+=("-executable=$BUNDLED_BINARY")
    fi
done < <(find "$APP_PLUGIN_DIR" "$APP_QML_DIR" -type f)

if command -v brew >/dev/null 2>&1; then
    BREW_PREFIX="$(brew --prefix)"

    # Homebrew uses split formulae and @rpath install names. macdeployqt also
    # checks this bundle-adjacent directory, so expose the formula symlinks
    # there during deployment. Nothing from it is shipped in the DMG.
    DEPLOY_SEARCH_DIR="$WORK_DIR/lib"
    BUNDLE_SEARCH_DIR="$APP/Contents/lib"
    case "$DEPLOY_SEARCH_DIR" in
        "$WORK_DIR/"*) ;;
        *) echo "Refusing unsafe deploy-search path: $DEPLOY_SEARCH_DIR" >&2; exit 1 ;;
    esac
    cmake -E rm -rf "$DEPLOY_SEARCH_DIR"
    cmake -E make_directory "$DEPLOY_SEARCH_DIR" "$BUNDLE_SEARCH_DIR"
    for DEPENDENCY in "$MOUFFETTE_QT_ROOT/lib/"*.framework "$BREW_PREFIX/lib/"*.dylib; do
        [[ -e "$DEPENDENCY" ]] || continue
        ln -s "$(realpath "$DEPENDENCY")" "$DEPLOY_SEARCH_DIR/$(basename "$DEPENDENCY")"
        if [[ "$DEPENDENCY" == *.dylib ]]; then
            ln -s "$(realpath "$DEPENDENCY")" "$BUNDLE_SEARCH_DIR/$(basename "$DEPENDENCY")"
        fi
    done
    DEPLOY_ARGS+=("-libpath=$DEPLOY_SEARCH_DIR")
fi
DEPLOY_LOG="$WORK_DIR/deployment.log"
if ! "$MACDEPLOYQT" "${DEPLOY_ARGS[@]}" >"$DEPLOY_LOG" 2>&1; then
    cat "$DEPLOY_LOG" >&2
    echo "macdeployqt failed." >&2
    exit 1
fi
if [[ -n "${DEPLOY_SEARCH_DIR:-}" ]]; then
    cmake -E rm -rf "$DEPLOY_SEARCH_DIR"
fi
if [[ -n "${BUNDLE_SEARCH_DIR:-}" ]]; then
    cmake -E rm -rf "$BUNDLE_SEARCH_DIR"
fi

# macdeployqt does not rewrite every transitive Homebrew install name (notably
# those first discovered through a QML plugin). Normalize those references to
# the copy already placed in Contents/Frameworks.
while IFS= read -r BUNDLED_BINARY; do
    file "$BUNDLED_BINARY" | grep -q 'Mach-O' || continue
    INSTALL_NAME="$(otool -D "$BUNDLED_BINARY" 2>/dev/null | tail -n +2 | head -1 || true)"
    case "$INSTALL_NAME" in
        /opt/homebrew/*|/usr/local/*)
            if [[ "$INSTALL_NAME" == *.framework/* ]]; then
                FRAMEWORK_RELATIVE="${INSTALL_NAME#*.framework/}"
                FRAMEWORK_NAME="$(basename "${INSTALL_NAME%%.framework/*}").framework"
                REPLACEMENT="@rpath/$FRAMEWORK_NAME/$FRAMEWORK_RELATIVE"
            else
                REPLACEMENT="@rpath/$(basename "$INSTALL_NAME")"
            fi
            if ! install_name_tool -id "$REPLACEMENT" "$BUNDLED_BINARY" 2>>"$DEPLOY_LOG"; then
                cat "$DEPLOY_LOG" >&2
                exit 1
            fi
            ;;
    esac
    while IFS= read -r DEPENDENCY; do
        case "$DEPENDENCY" in
            /opt/homebrew/*|/usr/local/*)
                if [[ "$DEPENDENCY" == *.framework/* ]]; then
                    FRAMEWORK_RELATIVE="${DEPENDENCY#*.framework/}"
                    FRAMEWORK_NAME="$(basename "${DEPENDENCY%%.framework/*}").framework"
                    REPLACEMENT="@rpath/$FRAMEWORK_NAME/$FRAMEWORK_RELATIVE"
                    BUNDLED_DEPENDENCY="$APP/Contents/Frameworks/$FRAMEWORK_NAME/$FRAMEWORK_RELATIVE"
                else
                    DEPENDENCY_NAME="$(basename "$DEPENDENCY")"
                    REPLACEMENT="@rpath/$DEPENDENCY_NAME"
                    BUNDLED_DEPENDENCY="$APP/Contents/Frameworks/$DEPENDENCY_NAME"
                fi
                if [[ ! -e "$BUNDLED_DEPENDENCY" ]]; then
                    echo "macdeployqt did not bundle $DEPENDENCY (needed by $BUNDLED_BINARY)." >&2
                    exit 1
                fi
                if ! install_name_tool -change "$DEPENDENCY" "$REPLACEMENT" \
                    "$BUNDLED_BINARY" 2>>"$DEPLOY_LOG"; then
                    cat "$DEPLOY_LOG" >&2
                    exit 1
                fi
                ;;
        esac
    done < <(otool -L "$BUNDLED_BINARY" | tail -n +2 | awk '{ print $1 }')
done < <(find "$APP" -type f)

if [[ -n "${MOUFFETTE_MACOS_SIGN_IDENTITY:-}" ]]; then
    echo "Signing with Developer ID: $MOUFFETTE_MACOS_SIGN_IDENTITY"
    SIGN_ARGS=(--force --options runtime --timestamp --sign "$MOUFFETTE_MACOS_SIGN_IDENTITY")
else
    echo "No Developer ID configured; applying an ad-hoc signature for local testing."
    SIGN_ARGS=(--force --sign -)
fi

# Sign nested code before its containing bundle. Avoid `--deep`: it hides
# ordering mistakes and can produce bundles that pass locally but fail Gatekeeper.
while IFS= read -r NESTED_BINARY; do
    if ! codesign "${SIGN_ARGS[@]}" "$NESTED_BINARY" >>"$DEPLOY_LOG" 2>&1; then
        cat "$DEPLOY_LOG" >&2
        exit 1
    fi
done < <(
    find "$APP/Contents" -type f -print0 \
        | xargs -0 file \
        | awk -F: '/Mach-O/ { print $1 }' \
        | grep -vF "$APP/Contents/MacOS/Mouffette" \
        | sort
)
while IFS= read -r NESTED_BUNDLE; do
    if ! codesign "${SIGN_ARGS[@]}" "$NESTED_BUNDLE" >>"$DEPLOY_LOG" 2>&1; then
        cat "$DEPLOY_LOG" >&2
        exit 1
    fi
done < <(find "$APP/Contents" -depth -type d \( -name '*.framework' -o -name '*.bundle' \))
if ! codesign "${SIGN_ARGS[@]}" "$APP" >>"$DEPLOY_LOG" 2>&1; then
    cat "$DEPLOY_LOG" >&2
    exit 1
fi

if ! codesign --verify --deep --strict --verbose=0 "$APP" 2>>"$DEPLOY_LOG"; then
    cat "$DEPLOY_LOG" >&2
    exit 1
fi

echo "Checking that the bundle has no development-machine dependencies..."
DEPENDENCY_REPORT="$(mktemp -t mouffette-dependencies.XXXXXX)"
while IFS= read -r binary; do
    if file "$binary" | grep -q 'Mach-O'; then
        otool -L "$binary" >>"$DEPENDENCY_REPORT"
    fi
done < <(find "$APP" -type f)
if grep -E "(/opt/homebrew|/usr/local|$CLIENT_ROOT)" "$DEPENDENCY_REPORT"; then
    echo "The staged app still references a development-machine library." >&2
    exit 1
fi

ditto "$APP" "$FINAL_STAGE_DIR/Mouffette.app"

VERSION="$(sed -nE 's/^MouffetteClient_VERSION:STATIC=(.+)$/\1/p' "$BUILD_DIR/CMakeCache.txt" | head -1)"
[[ -n "$VERSION" ]] || VERSION="1.0.0"
ARCH="$(uname -m)"
DMG="$PACKAGE_DIR/Mouffette-$VERSION-macos-$ARCH.dmg"
cmake -E rm -f "$DMG" "$DMG.sha256"
hdiutil create -volname "Mouffette $VERSION" -srcfolder "$APP" \
    -ov -format UDZO "$DMG"

if [[ -n "${MOUFFETTE_MACOS_SIGN_IDENTITY:-}" ]]; then
    codesign --force --timestamp --sign "$MOUFFETTE_MACOS_SIGN_IDENTITY" "$DMG"
fi

if [[ "$NOTARIZE" == true && -n "${MOUFFETTE_NOTARY_PROFILE:-}" ]]; then
    echo "Submitting DMG to Apple notarization..."
    xcrun notarytool submit "$DMG" --keychain-profile "$MOUFFETTE_NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
    xcrun stapler validate "$DMG"
else
    echo "Notarization skipped (no MOUFFETTE_NOTARY_PROFILE or --no-notarize)."
fi

shasum -a 256 "$DMG" >"$DMG.sha256"
echo "Production package created: $DMG"
echo "Checksum: $DMG.sha256"

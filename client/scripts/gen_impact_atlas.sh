#!/usr/bin/env bash
# gen_impact_atlas.sh
#
# Builds the glyph border atlas generator tool and runs it to produce
# resources/fonts/impact_border_atlas.gba.
#
# Run this once whenever you want to regenerate the atlas (e.g. after changing
# the stroke parameters or expanding the character set).  Then rebuild the app
# normally with ./build.sh — the new .gba will be bundled automatically.
#
# Usage:  ./scripts/gen_impact_atlas.sh [--ref-size 1000]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOL_SRC="$CLIENT_DIR/tools/gen_glyph_atlas"
TOOL_BUILD="$CLIENT_DIR/build_gen_atlas"
FONT_FILE="$CLIENT_DIR/resources/fonts/impact.ttf"
OUTPUT_FILE="$CLIENT_DIR/resources/fonts/impact_border_atlas.gba"

REF_SIZE="1000"
CHARS="32-126,160-255"
STEPS="5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100"

# Allow override of ref-size via argument
for arg in "$@"; do
    case "$arg" in
        --ref-size=*) REF_SIZE="${arg#*=}" ;;
        --chars=*)    CHARS="${arg#*=}" ;;
        --steps=*)    STEPS="${arg#*=}" ;;
    esac
done

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║         Glyph Border Atlas Generator             ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""
echo "  Font:      $FONT_FILE"
echo "  Output:    $OUTPUT_FILE"
echo "  Ref size:  ${REF_SIZE}px"
echo "  Chars:     $CHARS"
echo "  Steps:     $STEPS"
echo ""

if [ ! -f "$FONT_FILE" ]; then
    echo "❌ Font file not found: $FONT_FILE"
    exit 1
fi

# ── Step 1: Configure the generator tool ─────────────────────────────────────
echo "🔧 Configuring generator tool..."
mkdir -p "$TOOL_BUILD"
cmake -S "$TOOL_SRC" -B "$TOOL_BUILD" -DCMAKE_BUILD_TYPE=Release 2>&1 | grep -v "^--" || true

# ── Step 2: Build the generator tool ─────────────────────────────────────────
echo "🔨 Building generator tool..."
cmake --build "$TOOL_BUILD" --config Release -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

TOOL_BIN="$TOOL_BUILD/gen_glyph_atlas"
if [ ! -f "$TOOL_BIN" ]; then
    echo "❌ Tool binary not found at $TOOL_BIN"
    exit 1
fi

# ── Step 3: Run the generator ─────────────────────────────────────────────────
echo ""
echo "⚙️  Generating atlas (this may take 1–3 minutes)..."
echo ""

"$TOOL_BIN" \
    --font    "$FONT_FILE" \
    --output  "$OUTPUT_FILE" \
    --ref-size "$REF_SIZE" \
    --chars   "$CHARS" \
    --steps   "$STEPS"

echo ""
if [ -f "$OUTPUT_FILE" ]; then
    SIZE_KB=$(du -k "$OUTPUT_FILE" | cut -f1)
    echo "✅ Atlas written: $OUTPUT_FILE (${SIZE_KB} KB)"
    echo ""
    echo "Now rebuild the app with ./build.sh to bundle the new atlas."
else
    echo "❌ Generator ran but output file not found."
    exit 1
fi

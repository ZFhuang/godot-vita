#!/bin/bash

# =============================================================================
# Configuration
# =============================================================================
# BUILD_MODE: "full" = build both release and debug templates
#             "debug" = build debug template only (faster iteration)
# Can be overridden via environment variable, e.g.: BUILD_MODE=debug ./build_vita_export.sh
BUILD_MODE="${BUILD_MODE:-debug}"

# Validate BUILD_MODE
if [ "$BUILD_MODE" != "full" ] && [ "$BUILD_MODE" != "debug" ]; then
    echo "ERROR: Invalid BUILD_MODE '$BUILD_MODE'. Must be 'full' or 'debug'."
    exit 1
fi

echo "Build mode: $BUILD_MODE"

# =============================================================================
# Build release export template (only in full mode)
# =============================================================================
if [ "$BUILD_MODE" = "full" ]; then
    echo "=== Building release export template ==="
    scons platform=vita target=release tools=no verbose=no warnings=no werror=no debug_symbols=no
    # Verify release zip was generated before cleaning
    if [ ! -f "bin/vita_release.zip" ]; then
        echo "ERROR: bin/vita_release.zip not found, release build may have failed."
        exit 1
    fi
    # Preserve ELF file for crash analysis (addr2line needs symbols)
    # release target produces: godot.vita.opt.32 (no "debug" in name)
    RELEASE_ELF=$(ls temp-build/godot.vita.opt.[0-9]* 2>/dev/null | head -1)
    if [ -n "$RELEASE_ELF" ]; then
        cp "$RELEASE_ELF" bin/godot_vita_release.elf
        echo "Saved ELF with symbols: bin/godot_vita_release.elf"
    fi
else
    echo "=== Skipping release build (debug-only mode) ==="
fi

# =============================================================================
# Build debug export template (always built)
# =============================================================================
echo "=== Building debug export template ==="
scons platform=vita target=release_debug tools=no verbose=no warnings=all werror=no debug_symbols=yes
# Verify debug zip was generated before cleaning
if [ ! -f "bin/vita_debug.zip" ]; then
    echo "ERROR: bin/vita_debug.zip not found, debug build may have failed."
    exit 1
fi
# Preserve ELF file for crash analysis (addr2line needs symbols)
# release_debug target produces: godot.vita.opt.debug.32
DEBUG_ELF=$(ls temp-build/godot.vita.opt.debug.* 2>/dev/null | head -1)
if [ -n "$DEBUG_ELF" ]; then
    cp "$DEBUG_ELF" bin/godot_vita_debug.elf
    echo "Saved ELF with symbols: bin/godot_vita_debug.elf"
fi

# Get version from version.py
VERSION_MAJOR=$(grep "^major" version.py | cut -d'=' -f2 | tr -d ' ')
VERSION_MINOR=$(grep "^minor" version.py | cut -d'=' -f2 | tr -d ' ')
VERSION_PATCH=$(grep "^patch" version.py | cut -d'=' -f2 | tr -d ' ')
VERSION_STATUS=$(grep "^status" version.py | cut -d'=' -f2 | tr -d ' "')
if [ "$VERSION_PATCH" = "0" ]; then
    VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_STATUS}"
else
    VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.${VERSION_STATUS}"
fi

# Create output directory
OUTPUT_DIR="vita_template_package"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# Copy templates based on build mode
if [ "$BUILD_MODE" = "full" ]; then
    cp bin/vita_release.zip "$OUTPUT_DIR/"
fi
cp bin/vita_debug.zip "$OUTPUT_DIR/"

# Create version.txt
echo "$VERSION" > "$OUTPUT_DIR/version.txt"

# Create tpz archive
if [ "$BUILD_MODE" = "full" ]; then
    TPZ_NAME="godot_vita_template_${VERSION}.tpz"
else
    TPZ_NAME="godot_vita_template_${VERSION}_debug_only.tpz"
fi
cd "$OUTPUT_DIR"
if [ "$BUILD_MODE" = "full" ]; then
    zip -r "../bin/$TPZ_NAME" vita_release.zip vita_debug.zip version.txt
else
    zip -r "../bin/$TPZ_NAME" vita_debug.zip version.txt
fi
cd ..

rm -rf "$OUTPUT_DIR"

echo "Build complete: bin/$TPZ_NAME"

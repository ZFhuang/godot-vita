#!/bin/bash

# Clean previous build
rm -rf temp-build

# Build vita release export template
scons platform=vita target=release tools=no verbose=yes warnings=all werror=no debug_symbols=no
# Verify release zip was generated before cleaning
if [ ! -f "bin/vita_release.zip" ]; then
    echo "ERROR: bin/vita_release.zip not found, release build may have failed."
    exit 1
fi
# Clean intermediate build files (zip is already in bin/)
rm -rf temp-build

# Build vita debug export template (release_debug with DEBUG_ENABLED)
scons platform=vita target=release_debug tools=no verbose=yes warnings=all werror=no debug_symbols=no
# Verify debug zip was generated before cleaning
if [ ! -f "bin/vita_debug.zip" ]; then
    echo "ERROR: bin/vita_debug.zip not found, debug build may have failed."
    exit 1
fi
# Clean intermediate build files (zip is already in bin/)
rm -rf temp-build

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

# Copy both templates
cp bin/vita_release.zip "$OUTPUT_DIR/"
cp bin/vita_debug.zip "$OUTPUT_DIR/"

# Create version.txt
echo "$VERSION" > "$OUTPUT_DIR/version.txt"

# Create tpz archive
TPZ_NAME="godot_vita_template_${VERSION}.tpz"
cd "$OUTPUT_DIR"
zip -r "../bin/$TPZ_NAME" vita_release.zip vita_debug.zip version.txt
cd ..

rm -rf "$OUTPUT_DIR"

echo "Build complete: bin/$TPZ_NAME"
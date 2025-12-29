#!/bin/bash

# Clean previous build
rm -rf temp-build

# Build vita export template
scons platform=vita target=release tools=no verbose=yes warnings=all werror=no debug_symbols=no

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

# Copy vita_release.zip
cp bin/vita_release.zip "$OUTPUT_DIR/"

# Create version.txt
echo "$VERSION" > "$OUTPUT_DIR/version.txt"

# Create tpz archive
TPZ_NAME="godot_vita_template_${VERSION}.tpz"
cd "$OUTPUT_DIR"
zip -r "../bin/$TPZ_NAME" vita_release.zip version.txt
cd ..

rm -rf "$OUTPUT_DIR"

echo "Build complete: bin/$TPZ_NAME"
#!/usr/bin/env bash

set -e

PROJECT_DIR=~/pico/jamma64
BUILD_DIR=$PROJECT_DIR/build

echo "=== JAMMA64 Build Script ==="

# Ensure SDK path exists
if [ -z "$PICO_SDK_PATH" ]; then
    echo "ERROR: PICO_SDK_PATH not set"
    exit 1
fi

echo "Using SDK at: $PICO_SDK_PATH"

# Clean build
echo "Cleaning build directory..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo "Running CMake..."
cmake -G Ninja -DPICO_BOARD=pico2_w ..

# Build
echo "Building firmware..."
ninja jamma64

echo ""
echo "=== BUILD COMPLETE ==="
ls -lh jamma64.uf2
echo ""
echo "Copying to Windows Downloads..."
cp jamma64.uf2 "/mnt/c/Users/Tighe Lory/Downloads/"

echo "Done."
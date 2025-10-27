#!/bin/bash
# Master script to build all dependencies and samplecrate for Windows

set -e

echo "=========================================="
echo "Building all dependencies for Windows"
echo "=========================================="
echo ""

# Check if MinGW packages are installed
echo "Step 0: Checking MinGW packages..."
REQUIRED_PACKAGES="mingw64-gcc mingw64-gcc-c++ mingw64-SDL2 mingw64-filesystem"
MISSING=""

for pkg in $REQUIRED_PACKAGES; do
    if ! rpm -q $pkg &> /dev/null; then
        MISSING="$MISSING $pkg"
    fi
done

if [ -n "$MISSING" ]; then
    echo "Missing packages:$MISSING"
    echo ""
    echo "Install with:"
    echo "  sudo dnf install$MISSING"
    exit 1
fi

echo "✓ All required MinGW packages installed"
echo ""

# Build RtMidi
echo "=========================================="
echo "Step 1: Building RtMidi..."
echo "=========================================="
./build-rtmidi-mingw.sh

# Build sfizz
echo ""
echo "=========================================="
echo "Step 2: Building sfizz..."
echo "=========================================="
./build-sfizz-mingw.sh

# Build samplecrate
echo ""
echo "=========================================="
echo "Step 3: Building samplecrate..."
echo "=========================================="
./build-samplecrate-mingw.sh

echo ""
echo "=========================================="
echo "ALL BUILDS COMPLETED SUCCESSFULLY!"
echo "=========================================="
echo ""
echo "Next steps:"
echo "  1. Test the executable: wine build-windows/samplecrate.exe"
echo "  2. Package for distribution: ./package-windows.sh"
echo ""

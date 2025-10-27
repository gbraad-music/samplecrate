#!/bin/bash
# Script to package samplecrate for Windows distribution

set -e

VERSION="1.0.0"
BUILD_DIR="build-windows"
PACKAGE_NAME="samplecrate-${VERSION}-windows-x64"
DIST_DIR="$PACKAGE_NAME"
MINGW_SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"

echo "=========================================="
echo "Packaging samplecrate for Windows"
echo "=========================================="
echo ""

# Check if executable exists
if [ ! -f "$BUILD_DIR/samplecrate.exe" ]; then
    echo "Error: samplecrate.exe not found!"
    echo "Run ./build-samplecrate-mingw.sh first"
    exit 1
fi

# Create distribution directory
echo "Creating distribution directory: $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# Copy executable
echo "Copying executable..."
cp "$BUILD_DIR/samplecrate.exe" "$DIST_DIR/"

# Analyze DLL dependencies
echo "Analyzing DLL dependencies..."
if command -v x86_64-w64-mingw32-objdump &> /dev/null; then
    echo "Required DLLs:"
    x86_64-w64-mingw32-objdump -p "$BUILD_DIR/samplecrate.exe" | grep "DLL Name:" | sort -u
    echo ""
fi

# Copy required DLLs
echo "Copying required DLLs..."
echo ""

# SDL2 is required (dynamically linked)
if [ -f "$MINGW_SYSROOT/bin/SDL2.dll" ]; then
    cp "$MINGW_SYSROOT/bin/SDL2.dll" "$DIST_DIR/"
    echo "  ✓ SDL2.dll (dynamically linked)"
else
    echo "  ✗ SDL2.dll NOT FOUND - executable will not work!"
    echo "    Install: sudo dnf install mingw64-SDL2"
    exit 1
fi

# Copy all required DLLs found in the executable
echo "Checking for additional DLLs..."

# sfizz DLL
SFIZZ_DLL="/usr/x86_64-w64-mingw32/bin/libsfizz.dll"
if [ -f "$SFIZZ_DLL" ]; then
    cp "$SFIZZ_DLL" "$DIST_DIR/"
    echo "  ✓ libsfizz.dll"
else
    echo "  ✗ libsfizz.dll NOT FOUND!"
    exit 1
fi

# Check if we need threading library (usually required)
if [ -f "$MINGW_SYSROOT/bin/libwinpthread-1.dll" ]; then
    if x86_64-w64-mingw32-objdump -p "$BUILD_DIR/samplecrate.exe" 2>/dev/null | grep -q "libwinpthread-1.dll"; then
        cp "$MINGW_SYSROOT/bin/libwinpthread-1.dll" "$DIST_DIR/"
        echo "  ✓ libwinpthread-1.dll (threading support)"
    fi
fi

# Check all DLLs (exe and all DLLs we copied) for C++ runtime dependencies
echo "Checking for C++ runtime DLLs needed by dependencies..."

# libstdc++-6.dll (C++ standard library - needed by libsfizz.dll)
if [ -f "$MINGW_SYSROOT/bin/libstdc++-6.dll" ]; then
    cp "$MINGW_SYSROOT/bin/libstdc++-6.dll" "$DIST_DIR/"
    echo "  ✓ libstdc++-6.dll (C++ runtime for sfizz)"
fi

# libgcc_s_seh-1.dll (GCC runtime - needed by libsfizz.dll)
if [ -f "$MINGW_SYSROOT/bin/libgcc_s_seh-1.dll" ]; then
    cp "$MINGW_SYSROOT/bin/libgcc_s_seh-1.dll" "$DIST_DIR/"
    echo "  ✓ libgcc_s_seh-1.dll (GCC runtime for sfizz)"
fi

echo ""

# Copy configuration files if they exist
echo "Copying configuration files..."
[ -f "samplecrate.ini" ] && cp samplecrate.ini "$DIST_DIR/"

# Copy example files if they exist
echo "Copying examples..."
if [ -d "examples" ]; then
    cp -r examples "$DIST_DIR/"
fi

# Create README
echo "Creating README..."
cat > "$DIST_DIR/README.txt" <<EOF
Samplecrate for Windows v${VERSION}
=====================================

A multi-timbral sampler with effects processing.
EOF

# Create archive
echo ""
echo "Creating ZIP archive..."
zip -r "${PACKAGE_NAME}.zip" "$DIST_DIR"

echo ""
echo "=========================================="
echo "Package created successfully!"
echo "=========================================="
echo ""
echo "Distribution directory: $DIST_DIR/"
echo "ZIP archive: ${PACKAGE_NAME}.zip"
echo ""
echo "Contents:"
ls -lh "$DIST_DIR"
echo ""
echo "To test on Windows:"
echo "  1. Extract ${PACKAGE_NAME}.zip"
echo "  2. Run samplecrate.exe"
echo ""
echo "To test with Wine:"
echo "  cd $DIST_DIR && wine samplecrate.exe"
echo ""

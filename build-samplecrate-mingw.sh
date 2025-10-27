#!/bin/bash
# Script to cross-compile samplecrate for Windows

set -e

BUILD_DIR="build-windows"
MINGW_PREFIX="/usr/x86_64-w64-mingw32"

# Check if MinGW is installed
if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    echo "Error: MinGW-w64 not found. Install with:"
    echo "  sudo dnf install mingw64-gcc mingw64-gcc-c++"
    exit 1
fi

# Check for required MinGW packages
echo "Checking for required MinGW packages..."
REQUIRED_PACKAGES="mingw64-SDL2"
MISSING_PACKAGES=""

for pkg in $REQUIRED_PACKAGES; do
    if ! rpm -q $pkg &> /dev/null; then
        MISSING_PACKAGES="$MISSING_PACKAGES $pkg"
    fi
done

if [ -n "$MISSING_PACKAGES" ]; then
    echo "Missing packages:$MISSING_PACKAGES"
    echo "Install with: sudo dnf install$MISSING_PACKAGES"
    exit 1
fi

# Set PKG_CONFIG_PATH for cross-compilation
export PKG_CONFIG_PATH="$MINGW_PREFIX/lib/pkgconfig:/usr/x86_64-w64-mingw32/sys-root/mingw/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

echo "PKG_CONFIG_PATH: $PKG_CONFIG_PATH"
echo ""

# Verify dependencies are found
echo "Verifying pkg-config dependencies..."
for dep in sdl2 rtmidi sfizz; do
    if pkg-config --exists $dep 2>/dev/null; then
        echo "  ✓ $dep found: $(pkg-config --modversion $dep)"
    else
        echo "  ✗ $dep NOT FOUND"
        echo "    Make sure you've run build-rtmidi-mingw.sh and build-sfizz-mingw.sh"
        exit 1
    fi
done
echo ""

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake for Windows cross-compilation
echo "Configuring samplecrate for Windows..."
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$MINGW_PREFIX;/usr/x86_64-w64-mingw32/sys-root/mingw" \
    -DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config \
    -DCMAKE_FIND_ROOT_PATH="$MINGW_PREFIX;/usr/x86_64-w64-mingw32/sys-root/mingw"

# Build
echo ""
echo "Building samplecrate..."
make -j$(nproc)

echo ""
echo "=========================================="
echo "Build completed successfully!"
echo "Executable: $BUILD_DIR/samplecrate.exe"
echo "=========================================="
echo ""
echo "To package for Windows, you'll need to copy DLLs:"
echo "  1. SDL2.dll (from mingw64-SDL2 package)"
echo "  2. Any other runtime DLLs"
echo ""
echo "Create a Windows package:"
echo "  mkdir -p dist-windows"
echo "  cp $BUILD_DIR/samplecrate.exe dist-windows/"
echo "  cp /usr/x86_64-w64-mingw32/sys-root/mingw/bin/SDL2.dll dist-windows/"
echo "  # Copy any .rsx files, .sfz files, etc."
echo ""

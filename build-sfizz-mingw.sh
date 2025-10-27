#!/bin/bash
# Script to cross-compile sfizz for Windows

set -e

# Configuration
SFIZZ_VERSION="1.2.3"
SFIZZ_URL="https://github.com/sfztools/sfizz/releases/download/${SFIZZ_VERSION}/sfizz-${SFIZZ_VERSION}.tar.gz"
BUILD_DIR="build-sfizz-mingw"
INSTALL_PREFIX="/usr/x86_64-w64-mingw32"

# Check if MinGW is installed
if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    echo "Error: MinGW-w64 not found. Install with:"
    echo "  sudo dnf install mingw64-gcc mingw64-gcc-c++"
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download sfizz if not already downloaded
if [ ! -f "sfizz-${SFIZZ_VERSION}.tar.gz" ]; then
    echo "Downloading sfizz ${SFIZZ_VERSION}..."
    wget "$SFIZZ_URL" -O "sfizz-${SFIZZ_VERSION}.tar.gz"
fi

# Extract
echo "Extracting..."
tar xzf "sfizz-${SFIZZ_VERSION}.tar.gz"
cd "sfizz-${SFIZZ_VERSION}"

# Create build directory
mkdir -p build-mingw
cd build-mingw

# Configure with CMake for Windows cross-compilation
echo "Configuring sfizz for Windows..."
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../../../toolchain-mingw64.cmake \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DSFIZZ_JACK=OFF \
    -DSFIZZ_RENDER=OFF \
    -DSFIZZ_LV2=OFF \
    -DSFIZZ_VST=OFF \
    -DSFIZZ_AU=OFF \
    -DSFIZZ_DEMOS=OFF \
    -DSFIZZ_BENCHMARKS=OFF \
    -DSFIZZ_TESTS=OFF \
    -DSFIZZ_USE_VCPKG=OFF

# Build
echo "Building..."
make -j$(nproc)

# Install
echo "Installing to $INSTALL_PREFIX..."
sudo make install

# Create pkg-config file if it doesn't exist or is incorrect
PKGCONFIG_DIR="$INSTALL_PREFIX/lib/pkgconfig"
PKGCONFIG_FILE="$PKGCONFIG_DIR/sfizz.pc"

echo "Creating/updating pkg-config file..."
sudo mkdir -p "$PKGCONFIG_DIR"
sudo tee "$PKGCONFIG_FILE" > /dev/null <<EOF
prefix=$INSTALL_PREFIX
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: sfizz
Description: SFZ parser and synth library
Version: ${SFIZZ_VERSION}
Libs: -L\${libdir} -lsfizz -lwinmm -lole32 -lshlwapi
Cflags: -I\${includedir}
EOF

echo ""
echo "sfizz built successfully for Windows!"
echo "Installed to: $INSTALL_PREFIX"
echo "pkg-config file created at: $PKGCONFIG_FILE"
echo ""

#!/bin/bash
# Script to cross-compile samplecrate for Android

set -e

# Configuration
ANDROID_NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/25.2.9519653}"
ANDROID_ABI="arm64-v8a"  # Options: armeabi-v7a, arm64-v8a, x86, x86_64
ANDROID_API_LEVEL=23     # Minimum for MIDI API support
BUILD_DIR="build-android-${ANDROID_ABI}"
INSTALL_PREFIX="$(pwd)/android-libs/${ANDROID_ABI}"

# Check if Android NDK is available
if [ ! -d "$ANDROID_NDK" ]; then
    echo "Error: Android NDK not found at: $ANDROID_NDK"
    echo "Please set ANDROID_NDK_HOME or install Android NDK"
    echo ""
    echo "Download from: https://developer.android.com/ndk/downloads"
    echo "Or install via Android Studio SDK Manager"
    exit 1
fi

echo "Using Android NDK: $ANDROID_NDK"
echo "Target ABI: $ANDROID_ABI"
echo "API Level: $ANDROID_API_LEVEL"
echo ""

# Create install prefix directory
mkdir -p "$INSTALL_PREFIX"

# Build sfizz for Android
echo ""
echo "=== Building sfizz for Android ==="
if [ ! -f "$INSTALL_PREFIX/lib/libsfizz.a" ]; then
    SFIZZ_BUILD="build-sfizz-android-${ANDROID_ABI}"
    mkdir -p "$SFIZZ_BUILD"
    cd "$SFIZZ_BUILD"

    # Download sfizz if needed
    SFIZZ_VERSION="1.2.3"
    if [ ! -f "sfizz-${SFIZZ_VERSION}.tar.gz" ]; then
        wget "https://github.com/sfztools/sfizz/releases/download/${SFIZZ_VERSION}/sfizz-${SFIZZ_VERSION}.tar.gz"
        tar xzf "sfizz-${SFIZZ_VERSION}.tar.gz"
    fi

    cd "sfizz-${SFIZZ_VERSION}"
    mkdir -p build-android
    cd build-android

    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-${ANDROID_API_LEVEL}" \
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

    make -j$(nproc)
    make install
    cd ../../..
else
    echo "sfizz already built (skipping)"
fi

# Build RtMidi for Android
echo ""
echo "=== Building RtMidi for Android ==="
if [ ! -f "$INSTALL_PREFIX/lib/librtmidi.a" ]; then
    RTMIDI_BUILD="build-rtmidi-android-${ANDROID_ABI}"
    mkdir -p "$RTMIDI_BUILD"
    cd "$RTMIDI_BUILD"

    # Download RtMidi if needed
    RTMIDI_VERSION="6.0.0"
    if [ ! -f "rtmidi-${RTMIDI_VERSION}.tar.gz" ]; then
        wget "https://github.com/thestk/rtmidi/archive/refs/tags/${RTMIDI_VERSION}.tar.gz" -O "rtmidi-${RTMIDI_VERSION}.tar.gz"
        tar xzf "rtmidi-${RTMIDI_VERSION}.tar.gz"
    fi

    cd "rtmidi-${RTMIDI_VERSION}"
    mkdir -p build-android
    cd build-android

    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-${ANDROID_API_LEVEL}" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DRTMIDI_API_ANDROID=ON \
        -DRTMIDI_BUILD_TESTING=OFF

    make -j$(nproc)
    make install
    cd ../../..
else
    echo "RtMidi already built (skipping)"
fi

# Build SDL2 for Android
echo ""
echo "=== Building SDL2 for Android ==="
if [ ! -f "$INSTALL_PREFIX/lib/libSDL2.a" ]; then
    SDL2_BUILD="build-sdl2-android-${ANDROID_ABI}"
    mkdir -p "$SDL2_BUILD"
    cd "$SDL2_BUILD"

    # Download SDL2 if needed
    SDL2_VERSION="2.28.5"
    if [ ! -f "SDL2-${SDL2_VERSION}.tar.gz" ]; then
        wget "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"
        tar xzf "SDL2-${SDL2_VERSION}.tar.gz"
    fi

    cd "SDL2-${SDL2_VERSION}"
    mkdir -p build-android
    cd build-android

    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-${ANDROID_API_LEVEL}" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF

    make -j$(nproc)
    make install
    cd ../../..
else
    echo "SDL2 already built (skipping)"
fi

# Build samplecrate library for Android
echo ""
echo "=== Building samplecrate library for Android ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-${ANDROID_API_LEVEL}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$INSTALL_PREFIX" \
    -DSDL2_DIR="$INSTALL_PREFIX/lib/cmake/SDL2" \
    -DBUILD_ANDROID_LIBRARY=ON

make -j$(nproc)

cd ..

echo ""
echo "=========================================="
echo "Android build completed successfully!"
echo "=========================================="
echo ""
echo "Output library: $BUILD_DIR/libsamplecrate.so"
echo "Target ABI: $ANDROID_ABI"
echo "API Level: $ANDROID_API_LEVEL"
echo ""

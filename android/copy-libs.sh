#!/bin/bash
# Script to copy native libraries and SDL2 Java files to Android project
# Run this after building with build-android.sh

set -e

# Determine the project root (parent of android directory)
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

ANDROID_ABI="${1:-arm64-v8a}"
BUILD_DIR="build-android-${ANDROID_ABI}"
LIBS_DIR="android-libs/${ANDROID_ABI}"
ANDROID_JNI_DIR="android/app/src/main/jniLibs/${ANDROID_ABI}"
SDL2_SOURCE_DIR="build-sdl2-android-${ANDROID_ABI}/SDL2-2.28.5"

echo "=========================================="
echo "Copying libraries for Android"
echo "=========================================="
echo "ABI: $ANDROID_ABI"
echo ""

# Check if build exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory not found: $BUILD_DIR"
    echo "Please run ./build-android.sh first"
    exit 1
fi

# Create jniLibs directory
echo "Creating jniLibs directory..."
mkdir -p "$ANDROID_JNI_DIR"

# Copy libsamplecrate.so
echo "Copying libsamplecrate.so..."
if [ -f "$BUILD_DIR/libsamplecrate.so" ]; then
    cp "$BUILD_DIR/libsamplecrate.so" "$ANDROID_JNI_DIR/"
    echo "  ✓ libsamplecrate.so copied"
else
    echo "  ✗ Error: libsamplecrate.so not found in $BUILD_DIR"
    exit 1
fi

# Copy libSDL2.so
echo "Copying libSDL2.so..."
if [ -f "$LIBS_DIR/lib/libSDL2.so" ]; then
    cp "$LIBS_DIR/lib/libSDL2.so" "$ANDROID_JNI_DIR/"
    echo "  ✓ libSDL2.so copied"
else
    echo "  ✗ Error: libSDL2.so not found in $LIBS_DIR/lib"
    exit 1
fi

# Copy SDL2 Java files (only need to do this once)
if [ ! -d "android/app/src/main/java/org/libsdl/app/SDLActivity.java" ]; then
    echo ""
    echo "Copying SDL2 Java files..."
    if [ -d "$SDL2_SOURCE_DIR/android-project/app/src/main/java/org/libsdl" ]; then
        cp -r "$SDL2_SOURCE_DIR/android-project/app/src/main/java/org/libsdl" \
            android/app/src/main/java/org/
        echo "  ✓ SDL2 Java files copied"
    else
        echo "  ✗ Warning: SDL2 Java files not found in $SDL2_SOURCE_DIR"
        echo "  You'll need to copy them manually from the SDL2 source"
    fi
else
    echo ""
    echo "SDL2 Java files already present (skipping)"
fi

# Copy assets
echo ""
echo "Copying assets..."
if [ -d "assets" ]; then
    mkdir -p "android/app/src/main/assets"
    cp -r assets/* android/app/src/main/assets/
    echo "  ✓ Assets copied"
else
    echo "  ℹ No assets directory found (skipping)"
fi

echo ""
echo "=========================================="
echo "Libraries copied successfully!"
echo "=========================================="
echo ""
echo "You can now build the APK:"
echo "  cd android"
echo "  ./gradlew assembleDebug"
echo ""
echo "Or open the android/ directory in Android Studio"
echo ""

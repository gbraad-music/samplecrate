# SDL2 Java Files

This directory should contain the SDL2 Java interface files from the SDL2 source code.

## How to Copy SDL2 Java Files

After building SDL2 using `build-android.sh`, copy the Java files from the SDL2 source:

```bash
# Find the SDL2 source directory (created during build)
SDL2_SOURCE="build-sdl2-android-arm64-v8a/SDL2-2.28.5"

# Copy all SDL Java files to the Android project
cp -r "$SDL2_SOURCE/android-project/app/src/main/java/org/libsdl" \
    android/app/src/main/java/org/
```

## Required Files

The following files should be copied from `SDL2-x.x.x/android-project/app/src/main/java/org/libsdl/app/`:

- `SDLActivity.java` - Main SDL activity class
- `SDLAudioManager.java` - Audio handling
- `SDLControllerManager.java` - Game controller support
- `SDLSurface.java` - SDL rendering surface
- `HIDDevice*.java` - HID device support files

## Alternative: Direct Download

You can also download the SDL2 source from https://github.com/libsdl-org/SDL/releases and extract just the Java files.

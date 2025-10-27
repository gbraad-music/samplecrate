# Samplecrate Android App

This directory contains the Android Studio project for building Samplecrate as an Android APK.

## Prerequisites

1. **Android Studio** (Arctic Fox or newer)
2. **Android NDK** (version 25.x recommended)
3. **Android SDK** with API level 23+ (for MIDI support)
4. **Gradle** 8.0+ (usually bundled with Android Studio)

## Project Structure

```
android/
├── app/
│   ├── build.gradle              # App-level Gradle configuration
│   ├── src/main/
│   │   ├── AndroidManifest.xml   # App manifest with permissions
│   │   ├── java/
│   │   │   ├── com/samplecrate/app/
│   │   │   │   └── SamplecrateActivity.java  # Main activity
│   │   │   └── org/libsdl/app/   # SDL2 Java files (see below)
│   │   ├── res/                  # Android resources
│   │   ├── assets/               # Place .sfz files and samples here
│   │   └── jniLibs/              # Native libraries (.so files)
│   │       ├── arm64-v8a/
│   │       └── armeabi-v7a/
├── build.gradle                  # Project-level Gradle configuration
├── settings.gradle               # Gradle settings
└── gradle.properties             # Gradle properties
```

## Build Instructions

### Step 1: Build Native Libraries

First, build the native libraries using the cross-compilation script:

```bash
# From the project root directory
./build-android.sh
```

This will:
- Download and build SDL2, sfizz, and RtMidi for Android
- Build libsamplecrate.so for arm64-v8a architecture
- Place the output in `build-android-arm64-v8a/`

To build for multiple architectures, edit `build-android.sh` and change `ANDROID_ABI`:

```bash
# For 32-bit ARM
ANDROID_ABI="armeabi-v7a"

# For 64-bit ARM (default)
ANDROID_ABI="arm64-v8a"
```

### Step 2: Copy SDL2 Java Files

SDL2's Java interface files must be copied from the SDL2 source:

```bash
# Copy SDL2 Java files from the build directory
cp -r build-sdl2-android-arm64-v8a/SDL2-2.28.5/android-project/app/src/main/java/org/libsdl \
    android/app/src/main/java/org/
```

**Note:** You only need to do this once, unless you upgrade SDL2.

### Step 3: Copy Native Libraries

Copy the built native libraries to the Android project:

```bash
# Copy arm64-v8a libraries
mkdir -p android/app/src/main/jniLibs/arm64-v8a
cp build-android-arm64-v8a/libsamplecrate.so android/app/src/main/jniLibs/arm64-v8a/
cp android-libs/arm64-v8a/lib/libSDL2.so android/app/src/main/jniLibs/arm64-v8a/

# If you built for armeabi-v7a as well:
mkdir -p android/app/src/main/jniLibs/armeabi-v7a
cp build-android-armeabi-v7a/libsamplecrate.so android/app/src/main/jniLibs/armeabi-v7a/
cp android-libs/armeabi-v7a/lib/libSDL2.so android/app/src/main/jniLibs/armeabi-v7a/
```

### Step 4: Copy Assets

Copy your SFZ files and audio samples to the assets directory:

```bash
# Copy assets (adjust paths as needed)
cp -r assets/* android/app/src/main/assets/
```

### Step 5: Build APK

#### Option A: Using Android Studio

1. Open Android Studio
2. Select "Open an Existing Project"
3. Navigate to the `android/` directory
4. Wait for Gradle sync to complete
5. Click "Build" → "Build Bundle(s) / APK(s)" → "Build APK(s)"
6. The APK will be in `app/build/outputs/apk/debug/`

#### Option B: Using Command Line

```bash
cd android

# Debug build
./gradlew assembleDebug

# Release build (requires signing configuration)
./gradlew assembleRelease
```

The APK will be in `app/build/outputs/apk/debug/app-debug.apk`

### Step 6: Install on Device

```bash
# Install via adb
adb install app/build/outputs/apk/debug/app-debug.apk

# Or install and run
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.samplecrate.app/.SamplecrateActivity
```

## Development Tips

### Viewing Logs

Use `adb logcat` to view application logs:

```bash
# View all logs
adb logcat

# Filter for Samplecrate logs
adb logcat -s Samplecrate SDL

# Clear logs and view new ones
adb logcat -c && adb logcat -s Samplecrate SDL
```

### Debugging Native Code

1. In Android Studio, select "Run" → "Edit Configurations"
2. Select "Debugger" tab
3. Change "Debug type" to "Dual (Java + Native)"
4. Set breakpoints in your C++ code
5. Run in debug mode

### Performance Profiling

Use Android Studio's Profiler:
1. Run the app in debug mode
2. Open "View" → "Tool Windows" → "Profiler"
3. Monitor CPU, memory, and audio performance

## Permissions

The app requests the following permissions (see `AndroidManifest.xml`):

- `READ_EXTERNAL_STORAGE` - Read SFZ files and samples
- `WRITE_EXTERNAL_STORAGE` - Save settings (Android 12 and below)
- `READ_MEDIA_AUDIO` - Read audio files (Android 13+)
- USB Host - For USB MIDI controllers

## MIDI Support

The app supports MIDI input via:
- **USB MIDI** - Plug in a USB MIDI controller via OTG adapter
- **Bluetooth MIDI** - Pair Bluetooth MIDI devices in Android settings
- **Virtual MIDI** - Use apps like "MIDI Keyboard" as input

USB MIDI devices are auto-detected when plugged in (see `device_filter.xml`).

## Troubleshooting

### "SDL2 not found" error
- Ensure SDL2 Java files are copied to `app/src/main/java/org/libsdl/`
- Check that `libSDL2.so` is in `jniLibs/arm64-v8a/`

### "Unable to load native library" error
- Verify `libsamplecrate.so` is in `jniLibs/arm64-v8a/`
- Check ABI compatibility: run `adb shell getprop ro.product.cpu.abi`

### "No samples loaded" error
- Ensure assets are copied to `app/src/main/assets/`
- Check file permissions and paths in your C++ code

### App crashes on startup
- Check logcat for error messages: `adb logcat -s Samplecrate SDL AndroidRuntime`
- Verify all dependencies (sfizz, RtMidi) are statically linked

## Release Build

To create a signed release build:

1. Generate a keystore:
```bash
keytool -genkey -v -keystore samplecrate-release.keystore \
    -alias samplecrate -keyalg RSA -keysize 2048 -validity 10000
```

2. Create `app/keystore.properties`:
```properties
storeFile=/path/to/samplecrate-release.keystore
storePassword=your_password
keyAlias=samplecrate
keyPassword=your_password
```

3. Add signing configuration to `app/build.gradle`:
```gradle
android {
    signingConfigs {
        release {
            def keystorePropertiesFile = rootProject.file("app/keystore.properties")
            def keystoreProperties = new Properties()
            keystoreProperties.load(new FileInputStream(keystorePropertiesFile))

            storeFile file(keystoreProperties['storeFile'])
            storePassword keystoreProperties['storePassword']
            keyAlias keystoreProperties['keyAlias']
            keyPassword keystoreProperties['keyPassword']
        }
    }
    buildTypes {
        release {
            signingConfig signingConfigs.release
            // ... rest of release config
        }
    }
}
```

4. Build release APK:
```bash
./gradlew assembleRelease
```

## License

Same as the main Samplecrate project.

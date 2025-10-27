# Building Samplecrate for Windows (Cross-compilation from Fedora Linux)


This guide explains how to cross-compile samplecrate for Windows using MinGW on Fedora Linux.

## Prerequisites

### Install MinGW cross-compilation toolchain and dependencies

```bash
sudo dnf install y \
    mingw64-gcc \
    mingw64-gcc-c++ \
    mingw64-gcc-cpp \
    mingw64-SDL2 \
    mingw64-zlib \
    mingw64-zlib-static \
    cmake \
    wget \
    zip
```

## Build Process

### Option 1: Build Everything (Recommended for first time)

Run the master build script that builds all dependencies and samplecrate:

```bash
./build-windows.sh
```

This will:
1. Check for required packages
2. Build RtMidi for Windows
3. Build sfizz for Windows
4. Build samplecrate for Windows

### Option 2: Build Step-by-Step

If you need to rebuild individual components:


#### 1. Build sfizz
```bash
./build-sfizz-mingw.sh
```

This builds sfizz as a static library and creates pkg-config files.

#### 2. RtMidi with WinMM support
```bash
./build-rtmidi-mingw.sh
```

This builds RtMidi with WinMM backend and installs to `/usr/x86_64-w64-mingw32/`

#### 3. Build samplecrate
```bash
./build-samplecrate-mingw.sh
```

This builds the main samplecrate executable using the cross-compiled dependencies.

### 4. Package for distribution

To create a distributable package with all DLLs:

```bash
./package-windows.sh
```
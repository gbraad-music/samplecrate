cmake_minimum_required(VERSION 3.10)
project(samplecrate)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer GLVND for modern OpenGL (fixes CMake warning)
if(POLICY CMP0072)
    cmake_policy(SET CMP0072 NEW)
endif()

# Build options
option(BUILD_ANDROID_LIBRARY "Build as Android shared library" OFF)

# Platform detection
if(ANDROID)
    set(BUILD_ANDROID_LIBRARY ON)
    message(STATUS "Building for Android platform")
endif()

# Add /usr/local to search paths (not needed for Android)
if(NOT ANDROID)
    set(CMAKE_PREFIX_PATH "/usr/local;${CMAKE_PREFIX_PATH}")
    list(APPEND CMAKE_LIBRARY_PATH /usr/local/lib)
    list(APPEND CMAKE_INCLUDE_PATH /usr/local/include)

    # Set pkg-config to also search /usr/local
    set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

# Find required packages
if(ANDROID)
    # For Android, find libraries manually
    find_package(SDL2 REQUIRED CONFIG)
    find_library(OPENGL_LIBRARIES GLESv2)
    find_library(RTMIDI_LIBRARIES rtmidi REQUIRED)
    find_library(SFIZZ_LIBRARIES sfizz REQUIRED)

    # Set include directories for Android
    set(RTMIDI_INCLUDE_DIRS "${CMAKE_FIND_ROOT_PATH}/include")
    set(SFIZZ_INCLUDE_DIRS "${CMAKE_FIND_ROOT_PATH}/include")
else()
    # Desktop builds use pkg-config
    find_package(PkgConfig REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(RTMIDI REQUIRED rtmidi)
    pkg_check_modules(SFIZZ REQUIRED sfizz)
endif()

# ImGui source files
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)

# Choose OpenGL backend based on platform
if(ANDROID)
    set(IMGUI_OPENGL_BACKEND ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
else()
    set(IMGUI_OPENGL_BACKEND ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp)
endif()

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_OPENGL_BACKEND}
)

# Include directories
include_directories(
    ${SDL2_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
    ${RTMIDI_INCLUDE_DIRS}
    ${SFIZZ_INCLUDE_DIRS}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Source files
set(SAMPLECRATE_SOURCES
    main.cpp
    lcd.c
    samplecrate_common.c
    samplecrate_rsx.c
    regroove_effects.c
    midi.c
    input_mappings.c
    ${IMGUI_SOURCES}
)

# Build as library or executable
if(BUILD_ANDROID_LIBRARY)
    # Build as shared library for Android
    add_library(samplecrate SHARED ${SAMPLECRATE_SOURCES})

    # Android-specific compile definitions
    target_compile_definitions(samplecrate PRIVATE IMGUI_IMPL_OPENGL_ES2)
else()
    # Build as executable for desktop
    add_executable(samplecrate ${SAMPLECRATE_SOURCES})
endif()

# Link libraries
target_link_libraries(samplecrate
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${RTMIDI_LIBRARIES}
    ${SFIZZ_LIBRARIES}
)

# Android-specific libraries
if(ANDROID)
    target_link_libraries(samplecrate
        log       # Android logging
        android   # Android native API
    )
endif()

# Windows-specific settings
if(WIN32)
    # Enable console window to see error messages and stdout
    if(MINGW)
        target_link_options(samplecrate PRIVATE -mconsole)
    endif()

    # Additional Windows libraries that might be needed
    target_link_libraries(samplecrate
        winmm      # Windows Multimedia API (for MIDI/Audio)
        ole32      # COM support (may be needed by sfizz)
        shlwapi    # Shell API (may be needed by sfizz file operations)
    )
endif()

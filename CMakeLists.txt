cmake_minimum_required(VERSION 3.10)
project(samplecrate)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add /usr/local to search paths
set(CMAKE_PREFIX_PATH "/usr/local;${CMAKE_PREFIX_PATH}")
list(APPEND CMAKE_LIBRARY_PATH /usr/local/lib)
list(APPEND CMAKE_INCLUDE_PATH /usr/local/include)

# Set pkg-config to also search /usr/local
set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

# Find required packages
find_package(PkgConfig REQUIRED)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
pkg_check_modules(RTMIDI REQUIRED rtmidi)
pkg_check_modules(SFIZZ REQUIRED sfizz)

# ImGui source files
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
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

# Executable
add_executable(samplecrate
    main.cpp
    lcd.c
    samplecrate_common.c
    samplecrate_rsx.c
    regroove_effects.c
    midi.c
    input_mappings.c
    ${IMGUI_SOURCES}
)

# Link libraries
target_link_libraries(samplecrate
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${RTMIDI_LIBRARIES}
    ${SFIZZ_LIBRARIES}
)

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

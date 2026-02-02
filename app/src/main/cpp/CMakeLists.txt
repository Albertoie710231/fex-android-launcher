cmake_minimum_required(VERSION 3.22.1)

project("steamlauncher")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable position independent code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find required packages
find_library(log-lib log)
find_library(android-lib android)
find_library(EGL-lib EGL)
find_library(GLESv2-lib GLESv2)

# Main native library
add_library(steamlauncher SHARED
    steamlauncher.cpp
    lorie/lorie_server.cpp
    lorie/lorie_renderer.cpp
    lorie/lorie_input.cpp
    vulkan_bridge/vulkan_bridge.cpp
)

# Include directories
target_include_directories(steamlauncher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lorie
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_bridge
)

# Link libraries
target_link_libraries(steamlauncher
    ${log-lib}
    ${android-lib}
    ${EGL-lib}
    ${GLESv2-lib}
    jnigraphics
)

# Compiler flags
target_compile_options(steamlauncher PRIVATE
    -Wall
    -Wextra
    -O2
    -fvisibility=hidden
)

# PRoot library (if building from source)
# add_subdirectory(proot)

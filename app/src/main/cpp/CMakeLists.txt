cmake_minimum_required(VERSION 3.22.1)

project("steamlauncher")

# Minimal native library (X11 handled by Termux:X11)
find_library(log-lib log)

add_library(steamlauncher SHARED steamlauncher.cpp)

target_link_libraries(steamlauncher ${log-lib})

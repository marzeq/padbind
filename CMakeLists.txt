cmake_minimum_required(VERSION 3.27)
project(padbind VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    hyprland
    sdl3
    lua
    libdrm
    libinput
    libeis-1.0
    libudev
    pixman-1
    wayland-server
    xkbcommon
)

add_library(padbind SHARED src/padbind.cpp)
target_link_libraries(padbind PRIVATE PkgConfig::DEPS)
set_target_properties(padbind PROPERTIES PREFIX "" OUTPUT_NAME "padbind")
install(TARGETS padbind LIBRARY DESTINATION lib/hyprland/plugins)

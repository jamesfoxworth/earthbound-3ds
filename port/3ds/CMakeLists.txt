cmake_minimum_required(VERSION 3.13)

if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{DEVKITPRO})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake" CACHE PATH "")
endif()

project(earthbound_3ds C)
set(CMAKE_C_STANDARD 11)

# Milestone 1: native SNES viewport, top screen only. No widescreen, stereo
# 3D, or bottom-screen UI yet — those come once this boots and is playable.
set(EB_VIEWPORT_WIDTH 256 CACHE STRING "Viewport width in pixels")
set(EB_VIEWPORT_HEIGHT 224 CACHE STRING "Viewport height in pixels")

# Audio is a separate milestone (needs an ndsp backend for the SPC700/DSP
# emulator). Keeping it off for now reduces the surface area of this build.
set(EB_ENABLE_AUDIO OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# The 3DS top screen is configured as GSP_RGB565_OES, so build the game
# library's pixel construction (palette conversion, PIXEL_RGB) in RGB565
# channel order instead of the default BGR565 — otherwise red and blue are
# swapped. See the EB_PIXEL_RGB565 note in src/core/types.h.
#
# EB_PPU_FORCE_SPEED_OPT builds the software PPU renderer at -O3 rather than
# the toolchain default -O2, as the RP2040 port does.
target_compile_definitions(earthbound_game PUBLIC EB_PIXEL_RGB565=1)
target_compile_definitions(earthbound_game PRIVATE EB_PPU_FORCE_SPEED_OPT=1)

file(GLOB PLATFORM_SOURCES "platform/*.c")

add_executable(earthbound_3ds
    main.c
    ${PLATFORM_SOURCES}
)

target_include_directories(earthbound_3ds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(earthbound_3ds earthbound_game)

target_compile_options(earthbound_3ds PRIVATE -Wall -Wextra -Wno-unused-parameter)

ctr_generate_smdh("${CMAKE_CURRENT_BINARY_DIR}/earthbound_3ds.smdh"
    NAME "EarthBound"
    DESCRIPTION "EarthBound C port - 3DS homebrew (milestone 1)"
    AUTHOR "ebsrc contributors"
)

ctr_create_3dsx(earthbound_3ds
    SMDH "${CMAKE_CURRENT_BINARY_DIR}/earthbound_3ds.smdh"
)

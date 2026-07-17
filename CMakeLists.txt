# NOT the active build path. The build driver is tools/build.py (see
# docs/BUILDING.md): CMake's compiler-detection bootstrap does not survive a
# multi-word CMAKE_C_COMPILER value like "zig;cc" (it mis-splits the value,
# e.g. treating the drive letter of an absolute Windows path as a second
# list element). This file is kept as scaffolding/documentation of the
# intended target layout in case the build ever moves to CMake; it is not
# wired up to anything.
cmake_minimum_required(VERSION 3.20)

project(mp6native C CXX)

# compiler: clang (LLVM) on Windows (see docs/ARCHITECTURE.md)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MP6_SLICE_MENU "Build the boot-to-menu slice (game/ + bootDll + selmenuDll + fileseldll)" ON)

# TODO(platform lib): native implementations of the Dolphin SDK subsystems
# (GX/OS/VI/PAD/DVD/CARD) that live under platform/<subsystem>/, sitting
# behind the unchanged dolphin-API headers game code already includes.
# See docs/ARCHITECTURE.md for the full layering diagram.
#
# add_library(mp6-platform STATIC
#     platform/gx/...
#     platform/os/...
#     platform/vi/...
#     platform/pad/...
#     platform/dvd/...
#     platform/card/...
#     platform/audio/...
# )

# TODO(game lib): decompiled game sources consumed read-only from the
# sibling decomp repo (../../external_refs/repos/marioparty6, fork/main).
# See docs/DECOMP_DEPENDENCY.md. Gated by MP6_SLICE_MENU until board/ and
# the minigame RELs reach the same decomp standard as the boot-to-menu
# slice (game/ + bootDll + selmenuDll + fileseldll).
#
# if(MP6_SLICE_MENU)
#     add_library(mp6-game STATIC
#         # game/, bootDll, selmenuDll, fileseldll sources go here
#     )
# endif()

# TODO(mp6native executable): links mp6-game against mp6-platform (plus the
# shim/include compatibility headers) to produce the native port binary.
#
# add_executable(mp6native
#     # entry point source(s) go here
# )
# target_link_libraries(mp6native PRIVATE mp6-game mp6-platform)

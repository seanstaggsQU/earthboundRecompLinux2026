cmake_minimum_required(VERSION 3.16)
project(earthbound C)
set(CMAKE_C_STANDARD 11)

# Emit compile_commands.json so clangd/IDEs resolve include paths and types.
# A root-level symlink (../../compile_commands.json) points at this file.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Widescreen + overworld zoom: the desktop build is permanently compiled at
# a 512x256 max canvas -- both axes larger than the shipped 400x224
# widescreen baseline, earmarked for the overworld's runtime FOV/zoom
# cycle (see platform_video_set_zoom() in sdl2_video.c and ow.zoom_mode).
# This reuses the PPU's existing "wide mode"
# (BG_VIEWPORT_FILL/hardware-tilemap-bit rendering, EB_VIEWPORT_PAD_LEFT/TOP-
# based camera/entity/HUD offsets) already exercised by the Waveshare pico
# port at a different (narrower/shorter) fixed viewport size -- see
# src/core/types.h and src/snes/ppu.h.
# 512 = 64 tiles, the hard ceiling of the SNES-format horizontal (64x32)
# tilemap fill_tilemaps() already keeps fully resident regardless of
# viewport size -- see src/game/map_loader.c. 256 = 32 tiles, likewise the
# largest height that tilemap fill (and fill_collision_tiles()'s margin)
# already covers for free. Both are the exact safe maximum, not a budget
# with slack -- going further would need a bigger (64x64) tilemap layout,
# real VRAM/streaming work, out of scope here.
#
# The DEFAULT (non-zoomed) on-screen footprint stays exactly 400x224 --
# today's shipped widescreen baseline, unchanged -- via a presentation-layer
# crop in sdl2_video.c (EB_DEFAULT_WIDTH there), not by shrinking these
# compile-time constants. This used to be an EB_WIDESCREEN on/off build
# option; it's now always on, because the extra canvas is otherwise inert
# until the player actively zooms out in the overworld.
set(EB_VIEWPORT_WIDTH 512 CACHE STRING "Viewport width in pixels")
set(EB_VIEWPORT_HEIGHT 256 CACHE STRING "Viewport height in pixels")

# Build/version stamping. Best-effort: `git describe` gives a real tag or
# commit-ish string in a normal checkout; falls back to "dev" for a shallow
# clone, a source tarball with no .git, or any other failure. Generated into
# the build tree (never committed) so every build -- whether or not the
# self-updater below is configured -- has a real EB_VERSION_STRING to show
# in-game and compare against release tags.
find_package(Git QUIET)
set(EB_GIT_VERSION "dev")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _eb_git_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _eb_git_result)
    if(_eb_git_result EQUAL 0 AND _eb_git_describe)
        set(EB_GIT_VERSION "${_eb_git_describe}")
    endif()
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY)

# Build game library from src/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Find SDL2
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# Platform executable. Use GLOB_RECURSE (not plain GLOB): a non-recursive
# `file(GLOB ... CONFIGURE_DEPENDS ...)` silently returns empty on some
# filesystems (notably the Docker Desktop virtiofs mount used by the i386 build
# in port/unix/docker), which drops every platform/*.c object and breaks the
# link. GLOB_RECURSE + CONFIGURE_DEPENDS works there and matches src/CMakeLists.txt.
# platform/ has no subdirectories, so this is equivalent to a flat glob.
file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c")
add_executable(earthbound main.c ${PLATFORM_SOURCES})

target_link_libraries(earthbound earthbound_game)

if(TARGET SDL2::SDL2)
    target_link_libraries(earthbound SDL2::SDL2)
else()
    target_include_directories(earthbound PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(earthbound ${SDL2_LIBRARIES})
    target_link_directories(earthbound PRIVATE ${SDL2_LIBRARY_DIRS})
endif()

# Platform code needs SDL2 headers and game library headers (inherited via PUBLIC)
target_include_directories(earthbound PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

if(MSVC)
    target_compile_options(earthbound PRIVATE /W4)
else()
    target_compile_options(earthbound PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Self-update (port/unix/platform/sdl2_updater.c). Off/inert by default --
# matches every build before this feature existed -- unless both vars below
# are supplied at configure time, exactly like the EB_WINDOWS_ICON_RC hook
# further down: a packaging-time decision, never committed. EB_UPDATER_REPO
# is a plain "owner/repo" string (not a secret); EB_UPDATER_TOKEN is a
# fine-grained GitHub PAT (Contents:Read-only, scoped to that one repo) --
# see this file's own comment on the generated header below for why that
# specifically does NOT go through target_compile_definitions.
#
# macOS/Linux only for now (EB_UPDATER_ENABLED gates sdl2_updater.c's real
# implementation): Windows can't self-replace its own running .exe in the
# way this file's POSIX rename()-while-running approach relies on, and
# needs a WinHTTP backend instead of libcurl -- deferred to a follow-up.
set(EB_UPDATER_REPO "" CACHE STRING "owner/repo of the private GitHub releases repo (empty = updater disabled)")
set(EB_UPDATER_TOKEN "" CACHE STRING "Fine-grained GitHub PAT, Contents:Read-only, scoped to EB_UPDATER_REPO only (empty = updater disabled)")
if(EB_UPDATER_REPO AND EB_UPDATER_TOKEN AND NOT WIN32)
    # Generated into the build tree only, never committed -- configure_file()
    # (not target_compile_definitions) so the token never lands in
    # compile_commands.json, which gets copied into IDE configs and shared
    # build caches more often than expected. Included by exactly one
    # translation unit (sdl2_updater.c).
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/updater_secret.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/updater_secret.h
        @ONLY)
    target_compile_definitions(earthbound PRIVATE EB_UPDATER_ENABLED=1)

    find_package(CURL QUIET)
    if(NOT CURL_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(CURL REQUIRED libcurl)
    endif()
    if(TARGET CURL::libcurl)
        target_link_libraries(earthbound CURL::libcurl)
    else()
        target_include_directories(earthbound PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(earthbound ${CURL_LIBRARIES})
        target_link_directories(earthbound PRIVATE ${CURL_LIBRARY_DIRS})
    endif()
endif()

# winmm: timeBeginPeriod/timeEndPeriod (sdl2_timer.c) -- raises the process
# timer resolution so the manual frame-pacer's SDL_Delay() calls are precise
# enough not to fight the renderer's blocking VSync. Windows-only; on other
# platforms the default OS timer granularity doesn't have this problem.
if(WIN32)
    target_link_libraries(earthbound winmm)

    # Optional .exe icon resource. Empty/unset by default (inert -- no icon
    # embedded, matching every build before this option existed) since any
    # real icon worth shipping is a packaging-time decision made outside
    # this tree, not something to commit here: an EarthBound-themed icon is
    # derived from the game's own sprite art, which per this project's own
    # asset-provenance rules (see CLAUDE.md's "What Belongs in C Source
    # Code") must come from the donor ROM at build/pack time, not be
    # checked into source control as authored content. Pass
    # -DEB_WINDOWS_ICON_RC=/path/to/app.rc (a one-line ICON resource
    # script pointing at a .ico built from wherever the icon actually
    # lives) to opt in for a specific packaging run.
    set(EB_WINDOWS_ICON_RC "" CACHE FILEPATH "Optional .rc file embedding an .exe icon resource (not tracked in-repo)")
    if(EB_WINDOWS_ICON_RC)
        target_sources(earthbound PRIVATE ${EB_WINDOWS_ICON_RC})
    endif()
endif()

# Sanitizer support
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

set(_SANITIZERS "")
if(ENABLE_ASAN)
    list(APPEND _SANITIZERS "address")
endif()
if(ENABLE_UBSAN)
    list(APPEND _SANITIZERS "undefined")
endif()

if(_SANITIZERS)
    list(JOIN _SANITIZERS "," _SANITIZER_FLAGS)
    target_compile_options(earthbound PRIVATE -fsanitize=${_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    target_link_options(earthbound PRIVATE -fsanitize=${_SANITIZER_FLAGS})
endif()

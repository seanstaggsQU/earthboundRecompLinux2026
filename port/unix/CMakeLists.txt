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

# Optional: bake a prebuilt ebtools-setup binary for this exact platform
# directly into the game executable, so a player never needs a second file
# sitting next to the game to set itself up from a ROM -- and, critically,
# the self-updater (which only ever fetches the one game binary/lib) always
# carries a matching helper along automatically, with no separate download
# step of its own. Not part of a normal dev build: PyInstaller can't
# cross-compile and can't run as a CMake build step here (it has to run
# natively per target platform, separately, first) -- so this stays opt-in,
# wired up only by the release packaging process passing the path to an
# already-built ebtools-setup for this platform. A build configured without
# it falls back to looking for an ebtools-setup file on disk next to the
# game binary, same as before (see main.c).
set(EB_EMBED_SETUP_HELPER_PATH "" CACHE FILEPATH
    "Prebuilt ebtools-setup binary for this platform to embed in the game executable (release packaging only)")
if(EB_EMBED_SETUP_HELPER_PATH)
    if(NOT EXISTS "${EB_EMBED_SETUP_HELPER_PATH}")
        message(FATAL_ERROR "EB_EMBED_SETUP_HELPER_PATH set but file not found: ${EB_EMBED_SETUP_HELPER_PATH}")
    endif()
    set(EB_HAS_EMBEDDED_SETUP_HELPER ON)
endif()
set(EB_INCBIN_HEADER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vendor/incbin/incbin.h")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/embedded_helper.c.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_helper.c
    @ONLY)

# Build game library from src/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Find SDL2. Normal dev builds link dynamically against whatever SDL2 the
# system already has (fast, no extra build step). Release packaging instead
# points EB_SDL2_STATIC_CONFIG_DIR at a from-source SDL2 build configured
# with -DSDL_SHARED=OFF -DSDL_STATIC=ON (Homebrew's sdl2-compat package,
# the normal macOS dev dependency, only ships a dynamic library -- there's
# no static option to just find), so the release binary needs nothing
# alongside it at all, not even a bundled dylib/dll (this matters more on
# macOS/Windows; Linux has never needed a bundled SDL2 to begin with, since
# linking against the distro's own system library is the normal, expected
# thing there). The static SDL2 build's own generated CMake package already
# knows every macOS framework (CoreAudio, Cocoa, IOKit, etc.) it needs to
# pull in, so this just point find_package() at it instead of hand-listing
# frameworks ourselves.
set(EB_SDL2_STATIC_CONFIG_DIR "" CACHE PATH
    "Directory holding a from-source SDL2Config.cmake (SDL_SHARED=OFF/SDL_STATIC=ON build) to statically link instead of the system SDL2 (release packaging only)")
if(EB_SDL2_STATIC_CONFIG_DIR)
    find_package(SDL2 REQUIRED CONFIG PATHS "${EB_SDL2_STATIC_CONFIG_DIR}" NO_DEFAULT_PATH)
    set(EB_SDL2_TARGET SDL2::SDL2-static)
    # Lets sdl2_updater.c know this binary has no paired SDL2.dll/libSDL2
    # dylib to fetch/verify on update -- see its EB_SDL2_STATIC #ifdef.
    # Applied to the target below, once add_executable() has defined it.
    set(EB_SDL2_IS_STATIC TRUE)
else()
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    endif()
    set(EB_SDL2_TARGET SDL2::SDL2)
endif()

# Platform executable. Use GLOB_RECURSE (not plain GLOB): a non-recursive
# `file(GLOB ... CONFIGURE_DEPENDS ...)` silently returns empty on some
# filesystems (notably the Docker Desktop virtiofs mount used by the i386 build
# in port/unix/docker), which drops every platform/*.c object and breaks the
# link. GLOB_RECURSE + CONFIGURE_DEPENDS works there and matches src/CMakeLists.txt.
# platform/ has no subdirectories, so this is equivalent to a flat glob.
file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c")
add_executable(earthbound main.c ${PLATFORM_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_helper.c)
target_include_directories(earthbound PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# incbin.h embeds EB_EMBED_SETUP_HELPER_PATH's bytes via an assembler
# `.incbin` directive read directly from that file path at *compile* time,
# not through a normal C #include -- so CMake's automatic header-dependency
# scanning never sees it, and a rebuilt-but-same-named helper binary at that
# path silently keeps the OLD bytes baked into the executable until someone
# manually deletes the stale .o (bit us for real: a from-Aug-21 helper stayed
# embedded through several later fixes, including this session's own
# "Ass"->"Mario" rebuild, because nothing ever told CMake the input changed).
# OBJECT_DEPENDS makes that dependency explicit, so `cmake --build` alone
# (no manual .o deletion) recompiles embedded_helper.c whenever the prebuilt
# helper it points at is newer.
if(EB_HAS_EMBEDDED_SETUP_HELPER)
    set_source_files_properties(
        ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_helper.c
        PROPERTIES OBJECT_DEPENDS "${EB_EMBED_SETUP_HELPER_PATH}")
endif()

target_link_libraries(earthbound earthbound_game)

if(EB_SDL2_IS_STATIC)
    target_compile_definitions(earthbound PRIVATE EB_SDL2_STATIC)
endif()

if(TARGET ${EB_SDL2_TARGET})
    target_link_libraries(earthbound ${EB_SDL2_TARGET})
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

if(APPLE)
    # The linked libSDL2 dylib's own install_name is whatever absolute path
    # it was built with (e.g. Homebrew's /opt/homebrew/opt/sdl2-compat/lib/
    # libSDL2-2.0.0.dylib) -- fine for local dev on the machine that has it
    # installed, but this binary also ships as a public release asset
    # alongside its own libSDL2-2.0.0.dylib copy (see the release process),
    # meant to be dropped in the same folder as the executable on someone
    # else's Mac. Without this rewrite, the binary would only ever look at
    # the original absolute Homebrew path and fail to launch (dyld: image
    # not found) for anyone who doesn't happen to have that exact package
    # installed, regardless of the dylib sitting right next to it. Rewrite
    # the load command to @executable_path so it finds the shipped copy.
    find_program(INSTALL_NAME_TOOL install_name_tool)
    if(INSTALL_NAME_TOOL)
        add_custom_command(TARGET earthbound POST_BUILD
            COMMAND bash -c "for lib in $(otool -L $<TARGET_FILE:earthbound> | grep -o '/[^ ]*libSDL2[^ ]*dylib' || true); do ${INSTALL_NAME_TOOL} -change \"$lib\" @executable_path/libSDL2-2.0.0.dylib $<TARGET_FILE:earthbound>; done"
            COMMENT "Rewriting libSDL2 load path to @executable_path for redistribution"
            VERBATIM
        )
    endif()
endif()

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
# Windows uses WinHTTP (ships with the OS/MinGW-w64 headers, no extra
# dependency) instead of libcurl, and a detached helper-script relaunch
# instead of the POSIX rename()-while-running + execv() approach, since
# Windows locks its own running .exe and can't replace or re-exec it
# in-process -- see sdl2_updater.c's WIN32 branches.
set(EB_UPDATER_REPO "" CACHE STRING "owner/repo of the private GitHub releases repo (empty = updater disabled)")
set(EB_UPDATER_TOKEN "" CACHE STRING "Fine-grained GitHub PAT, Contents:Read-only, scoped to EB_UPDATER_REPO only (empty = updater disabled)")
if(EB_UPDATER_REPO AND EB_UPDATER_TOKEN)
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

    if(WIN32)
        target_link_libraries(earthbound winhttp)
    else()
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

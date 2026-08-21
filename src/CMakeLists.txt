cmake_minimum_required(VERSION 3.16)
project(earthbound_game C)
set(CMAKE_C_STANDARD 11)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "core/*.c"
    "entity/*.c"
    "game/*.c"
    "intro/*.c"
    "platform/*.c"
    "snes/*.c"
    "verify/*.c"
    "game_main.c"
)
file(GLOB_RECURSE DATA_SOURCES CONFIGURE_DEPENDS "data/*.c")
# data/tests/ holds standalone test harnesses with their own main() (e.g.
# test_runtime_assets.c) -- not part of the game library. data/runtime_generated/
# defines embedded_assets[] for EB_RUNTIME_ASSETS builds -- added explicitly,
# conditionally, below; picking it up here too would double-define that
# symbol alongside embedded_assets_array.c's in a default (non-runtime) build.
list(FILTER DATA_SOURCES EXCLUDE REGEX "/data/tests/|/data/runtime_generated/")

# tamp (savestate compression) is a git submodule; list its sources explicitly —
# the upstream repo also carries tests/benchmarks/bindings that must not build,
# and compressor_find_match_desktop.c is #include'd by compressor.c on 64-bit
# hosts, never compiled as its own translation unit.
set(TAMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tamp/tamp/_c_src")
if(NOT EXISTS "${TAMP_DIR}/tamp/compressor.c")
    message(FATAL_ERROR "tamp submodule missing — run: git submodule update --init")
endif()
list(APPEND SOURCES
    "${TAMP_DIR}/tamp/common.c"
    "${TAMP_DIR}/tamp/compressor.c"
    "${TAMP_DIR}/tamp/decompressor.c"
)

# cJSON (GitHub Releases API parsing) and sha256 (release-zip checksum
# verification) -- both single-file, vendored for the self-update feature
# (port/unix/platform/sdl2_updater.c). Built unconditionally like tamp
# above, not gated behind EB_UPDATER_ENABLED: they're small, dependency-free,
# and it keeps this list simple; the updater code that actually calls them
# is itself the part that's conditionally compiled.
list(APPEND SOURCES
    "vendor/cjson/cJSON.c"
    "vendor/sha256/sha256.c"
)
# Third-party code, not written to this project's -Wall -Wextra -- same
# precedent as the generated-embedded-assets source below (line ~275).
set_source_files_properties(
    "vendor/cjson/cJSON.c"
    "vendor/sha256/sha256.c"
    PROPERTIES COMPILE_FLAGS "-w"
)

add_library(earthbound_game STATIC ${SOURCES} ${DATA_SOURCES})

# libm: sin/cos/atan2/round etc. (entity.c, door.c, overworld_teleport.c,
# callroutine_movement.c). On glibc, GCC will also fuse adjacent sin()+cos()
# calls on the same argument into a single sincos() -- also from libm, so
# this is required there too. macOS's libSystem already provides these
# implicitly (no separate libm needed), which is why this went unnoticed
# there; find_library() correctly no-ops when there's nothing to add.
find_library(M_LIBRARY m)
if(M_LIBRARY)
    # Plain (non-keyword) signature to match the other
    # target_link_libraries(earthbound_game ...) calls below -- CMake
    # rejects mixing keyword and plain signatures for the same target.
    target_link_libraries(earthbound_game ${M_LIBRARY})
endif()

# Viewport resolution (defaults to SNES native 256x224)
set(EB_VIEWPORT_WIDTH 256 CACHE STRING "Viewport width in pixels")
set(EB_VIEWPORT_HEIGHT 224 CACHE STRING "Viewport height in pixels")

target_include_directories(earthbound_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    include/
    ${TAMP_DIR}
    vendor/cjson/
    vendor/sha256/
)

target_compile_definitions(earthbound_game PUBLIC
    USA=1
    EB_VIEWPORT_WIDTH=${EB_VIEWPORT_WIDTH}
    EB_VIEWPORT_HEIGHT=${EB_VIEWPORT_HEIGHT}
)

if(MSVC)
    target_compile_options(earthbound_game PRIVATE /W4)
else()
    target_compile_options(earthbound_game PRIVATE -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)
endif()

# Embedded assets - game assets are either loaded at startup from an
# assets.pak file (EB_RUNTIME_ASSETS=ON, the default -- see
# docs/get-your-own-assets.md) or compiled into the binary at build time
# via incbin.h (EB_RUNTIME_ASSETS=OFF, a maintainer-only dev path that
# needs a ROM and ebtools -- see docs/assets.md). Only that OFF path (plus
# the always-available test_runtime_assets fixture target, which isn't
# part of the default `cmake --build`) actually needs ebtools at configure
# time, so it's not REQUIRED here -- the default build never needs Python
# on the machine building it.
find_program(EBTOOLS_EXE ebtools)
set(EMBED_INCBIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/incbin")
# Shared generated-file output dir: used for asset embedding below (in
# non-runtime mode) *and* unconditionally further down for music/items
# header codegen, which doesn't depend on the asset-embedding mode -- so
# earthbound_game needs it on its include path regardless of asset mode.
set(EMBED_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(earthbound_game PUBLIC ${EMBED_GEN_DIR})

# Audio subsystem option (declared early: both the embed-registry exclusions
# below and the lakesnes_audio target further down need it)
option(EB_ENABLE_AUDIO "Enable audio subsystem (lakesnes SPC700/DSP emulator)" ON)

option(EB_RUNTIME_ASSETS
    "Load assets from an assets.pak file at startup instead of embedding them at compile time. \
This is the default and the only mode used for release builds -- the game builds its own pak \
from a player-supplied ROM at first launch (see src/data/rom_extract.c). Turn this OFF only for \
a one-off local dev build with a ROM already on hand -- see docs/assets.md."
    ON
)

if(NOT EB_RUNTIME_ASSETS AND NOT EBTOOLS_EXE)
    message(FATAL_ERROR "EB_RUNTIME_ASSETS=OFF needs ebtools on PATH (pip install -e . in a venv). "
        "Or drop -DEB_RUNTIME_ASSETS=OFF to use the default no-ROM-needed build.")
endif()

if(EB_RUNTIME_ASSETS)
    # --- Loader self-test: builds and runs against its own small checked-in
    # fixture (src/data/tests/fixtures/), independent of any ROM, so the
    # C loader can be sanity-checked before a real assets.pak exists. ---
    set(RT_TEST_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/tests/fixtures")
    set(RT_TEST_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime_assets_test/generated")
    set(RT_TEST_PAK "${CMAKE_CURRENT_BINARY_DIR}/runtime_assets_test/assets_test.pak")
    set(RT_TEST_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime_assets_test/scratch")
    file(MAKE_DIRECTORY ${RT_TEST_SCRATCH_DIR})

    add_custom_command(
        OUTPUT
            "${RT_TEST_GEN_DIR}/asset_ids.h"
            "${RT_TEST_GEN_DIR}/embedded_assets.h"
            "${RT_TEST_GEN_DIR}/asset_pack_layout.c"
            "${RT_TEST_GEN_DIR}/asset_pack_layout.h"
        COMMAND ${EBTOOLS_EXE} embed-registry --runtime
            ${RT_TEST_FIXTURE_DIR}/assets.manifest
            ${RT_TEST_FIXTURE_DIR}/bin
            ${RT_TEST_GEN_DIR}
            ${EMBED_INCBIN_DIR}
            --locale US
        DEPENDS ${RT_TEST_FIXTURE_DIR}/assets.manifest
        COMMENT "Generating runtime asset loader test fixture metadata"
    )
    add_custom_command(
        OUTPUT ${RT_TEST_PAK}
        COMMAND ${EBTOOLS_EXE} pack assets
            ${RT_TEST_FIXTURE_DIR}/assets.manifest
            ${RT_TEST_FIXTURE_DIR}/bin
            ${RT_TEST_PAK}
            --locale US
        DEPENDS ${RT_TEST_FIXTURE_DIR}/assets.manifest
        COMMENT "Packing runtime asset loader test fixture"
    )
    add_custom_target(runtime_assets_test_pak DEPENDS ${RT_TEST_PAK})

    add_executable(test_runtime_assets
        data/runtime_assets.c
        data/tests/test_runtime_assets.c
        "${RT_TEST_GEN_DIR}/asset_pack_layout.c"
    )
    target_include_directories(test_runtime_assets PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/data
        ${RT_TEST_GEN_DIR}
    )
    target_compile_definitions(test_runtime_assets PRIVATE EB_RUNTIME_ASSETS=1)
    add_dependencies(test_runtime_assets runtime_assets_test_pak)
    # Not auto-run by `cmake --build` -- invoke it manually:
    #   ./test_runtime_assets <path-to-RT_TEST_PAK> <path-to-RT_TEST_SCRATCH_DIR>

    # --- Main game target: needs pre-generated, checked-in metadata, since
    # a release/CI machine building this target has no ROM by design. This
    # is a WARNING, not a configure-time error: a missing-metadata checkout
    # should still let test_runtime_assets (above) configure and build --
    # only earthbound_game/earthbound actually need it, and they'll fail
    # at build time (missing headers) if it's absent, which is fine for a
    # checkout that only wants the loader self-test. ---
    set(RUNTIME_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/runtime_generated")
    set(EMBED_ASSET_HEADERS_DIR ${RUNTIME_GEN_DIR})
    set(EMBED_GENERATED_SOURCE "")

    if(NOT EXISTS "${RUNTIME_GEN_DIR}/asset_ids.h" OR NOT EXISTS "${RUNTIME_GEN_DIR}/asset_pack_layout.c"
        OR NOT EXISTS "${RUNTIME_GEN_DIR}/rom_extract_table.c")
        message(WARNING
            "EB_RUNTIME_ASSETS=ON: no pre-generated metadata in ${RUNTIME_GEN_DIR} "
            "yet, so earthbound_game/earthbound won't build until a maintainer "
            "with a legal EarthBound (USA) ROM generates and commits it:\n"
            "  ebtools extract earthbound.yml <rom> -d asm/bin\n"
            "  ebtools embed-registry --runtime asm/bin/assets.manifest asm/bin "
            "${RUNTIME_GEN_DIR} ${EMBED_INCBIN_DIR} --locale US\n"
            "(test_runtime_assets, configured above, is unaffected -- it uses "
            "its own small fixture instead.) See docs/assets.md."
        )
    else()
        # data/runtime_assets.c and data/rom_extract.c are already part of
        # earthbound_game via the top-level DATA_SOURCES glob (they're
        # no-op translation units when EB_RUNTIME_ASSETS isn't defined) --
        # only the generated files are new.
        target_sources(earthbound_game PRIVATE
            "${RUNTIME_GEN_DIR}/asset_pack_layout.c"
            "${RUNTIME_GEN_DIR}/rom_extract_table.c"
        )
        target_include_directories(earthbound_game PUBLIC ${RUNTIME_GEN_DIR})
        target_compile_definitions(earthbound_game PUBLIC EB_RUNTIME_ASSETS=1)
        set(EMBED_GENERATED_SOURCE "${RUNTIME_GEN_DIR}/asset_pack_layout.c")
    endif()
else()
    set(EMBED_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asm/bin")
    set(EMBED_MANIFEST "${EMBED_BIN_DIR}/assets.manifest")

    # Packed assets: human-friendly exports (assets/) are packed back to binary
    # in the build directory and used as overrides over asm/bin/.
    set(EMBED_PACKED_DIR "${CMAKE_CURRENT_BINARY_DIR}/packed_assets")
    set(ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set(PACK_ALL_STAMP "${CMAKE_CURRENT_BINARY_DIR}/pack_all.stamp")
    set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

    set(EMBED_ARRAY "${EMBED_GEN_DIR}/embedded_assets_array.c")
    set(EMBED_INC "${EMBED_GEN_DIR}/embedded_assets.inc.c")
    set(EMBED_HEADER "${EMBED_GEN_DIR}/embedded_assets.h")
    set(EMBED_IDS "${EMBED_GEN_DIR}/asset_ids.h")
    set(EMBED_DECL "${EMBED_GEN_DIR}/embedded_assets_decl.h")

    # Glob all human-friendly asset files so pack-all re-runs when they change
    file(GLOB_RECURSE ASSET_SOURCE_FILES CONFIGURE_DEPENDS "${ASSETS_DIR}/*")

    add_custom_command(
        OUTPUT ${PACK_ALL_STAMP}
        COMMAND ${EBTOOLS_EXE} pack-all
            ${ASSETS_DIR}
            ${EMBED_PACKED_DIR}
            -b ${EMBED_BIN_DIR}
            -y ${PROJECT_ROOT}/earthbound.yml
            -c ${PROJECT_ROOT}/commondefs.yml
        COMMAND ${CMAKE_COMMAND} -E touch ${PACK_ALL_STAMP}
        DEPENDS ${ASSET_SOURCE_FILES}
        COMMENT "Packing all human-friendly assets to binary"
    )

    # Optional asset exclusions (e.g., audio packs when audio is disabled).
    # ebtools/default_asset_excludes.py's DEFAULT_ASSET_EXCLUDES mirrors
    # everything below except the EB_ENABLE_AUDIO-conditional one (that
    # part only matters for audio-disabled embedded ports, never the
    # runtime-assets desktop build) -- keep both lists in sync if you
    # change either.
    set(EMBED_EXCLUDE_ARGS "")
    if(NOT EB_ENABLE_AUDIO)
        list(APPEND EMBED_EXCLUDE_ARGS --exclude "audiopacks/*")
    endif()

    # Exclude individual overworld sprite GFX files -- the C port loads
    # pre-packed bank files (overworld_sprites/banks/*.bin) instead.
    list(APPEND EMBED_EXCLUDE_ARGS --exclude "overworld_sprites/gfx/*")
    list(APPEND EMBED_EXCLUDE_ARGS --exclude "US/overworld_sprites/gfx/*")

    # Exclude monolithic PSI arrangement .arr.lzhal files -- the C port uses
    # .arr.bundled (8-frame bundles) for streaming decompression instead.
    list(APPEND EMBED_EXCLUDE_ARGS --exclude "psianims/arrangements/*.arr.lzhal")

    # Exclude legacy per-block text bytecode binaries -- all text is now served
    # from the unified dialogue.bin blob compiled from src/assets/dialogue/ YAML.
    list(APPEND EMBED_EXCLUDE_ARGS
        --exclude "data/EEVENT*.bin"
        --exclude "data/EBATTLE*.bin"
        --exclude "data/EGOODS*.bin"
        --exclude "data/ESYSTEM.bin"
        --exclude "data/EGLOBAL.bin"
        --exclude "data/EHINT.bin"
        --exclude "data/ENEWS.bin"
        --exclude "data/ESHOP*.bin"
        --exclude "data/EEXPL*.bin"
        --exclude "data/EBGMESS.bin"
        --exclude "data/EDEBUG.bin"
        --exclude "data/KEYBOARD.bin"
        --exclude "data/DEBUG_TEXT.bin"
        --exclude "data/DOOR_SCRIPTS.bin"
        --exclude "data/UNKNOWN_EFA2FA.bin"
        --exclude "data/E0[1-9]*.bin"
        --exclude "data/E1[0-9]*.bin"
        --exclude "dialogue/addr_remap.bin"
        --exclude "data/compressed_text_data.bin"
        --exclude "data/compressed_text_ptrs.bin"
    )

    add_custom_command(
        OUTPUT ${EMBED_ARRAY} ${EMBED_INC} ${EMBED_HEADER} ${EMBED_IDS} ${EMBED_DECL}
        COMMAND ${EBTOOLS_EXE} embed-registry
            ${EMBED_MANIFEST}
            ${EMBED_BIN_DIR}
            ${EMBED_GEN_DIR}
            ${EMBED_INCBIN_DIR}
            --custom-dir ${EMBED_PACKED_DIR}
            --locale US
            ${EMBED_EXCLUDE_ARGS}
        DEPENDS ${EMBED_MANIFEST} ${PACK_ALL_STAMP}
        COMMENT "Generating embedded asset registry"
    )

    target_sources(earthbound_game PRIVATE ${EMBED_ARRAY})
    # (include dir already added unconditionally above)

    set(EMBED_ASSET_HEADERS_DIR ${EMBED_GEN_DIR})
    set(EMBED_GENERATED_SOURCE ${EMBED_ARRAY})
endif()

# Suppress warnings in generated code (empty when EB_RUNTIME_ASSETS=ON but
# metadata hasn't been generated yet -- see the WARNING above)
if(EMBED_GENERATED_SOURCE)
    set_source_files_properties(${EMBED_GENERATED_SOURCE} PROPERTIES COMPILE_FLAGS "-w")
endif()


if(EB_ENABLE_AUDIO)
    add_library(lakesnes_audio STATIC
        vendor/lakesnes/snes/apu.c
        vendor/lakesnes/snes/spc.c
        vendor/lakesnes/snes/dsp.c
        vendor/lakesnes/snes/statehandler.c
    )
    target_include_directories(lakesnes_audio PRIVATE vendor/lakesnes/snes/)
    target_compile_options(lakesnes_audio PRIVATE -w)

    target_link_libraries(earthbound_game lakesnes_audio)
    target_include_directories(earthbound_game PUBLIC vendor/lakesnes/snes/)
    target_compile_definitions(earthbound_game PUBLIC EB_ENABLE_AUDIO=1)
endif()

# Side-by-side ROM verification using LakeSnes emulator
option(EB_ENABLE_VERIFY "Enable side-by-side ROM verification" OFF)

if(EB_ENABLE_VERIFY)
    file(GLOB LAKESNES_SOURCES "vendor/lakesnes/snes/*.c")
    add_library(lakesnes STATIC ${LAKESNES_SOURCES})
    target_include_directories(lakesnes PRIVATE vendor/lakesnes/)
    target_compile_options(lakesnes PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lakesnes/lk_prefix.h
        -w
    )
    target_compile_definitions(lakesnes PRIVATE EB_ENABLE_VERIFY=1)

    target_link_libraries(earthbound_game lakesnes)
    target_compile_definitions(earthbound_game PUBLIC EB_ENABLE_VERIFY=1)
    target_include_directories(earthbound_game PUBLIC vendor/lakesnes/)
endif()

# Generate music_generated.h from music_tracks.json
set(MUSIC_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/music_tracks.json")
set(MUSIC_HEADER "${EMBED_GEN_DIR}/music_generated.h")

add_custom_command(
    OUTPUT ${MUSIC_HEADER}
    COMMAND ${EBTOOLS_EXE} generate music-header
        ${MUSIC_JSON} ${MUSIC_HEADER}
    DEPENDS ${MUSIC_JSON}
    COMMENT "Generating music_generated.h"
)
add_custom_target(music_header DEPENDS ${MUSIC_HEADER})

# Generate items_generated.h from items.json
set(ITEMS_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/items/items.json")
set(ITEMS_HEADER "${EMBED_GEN_DIR}/items_generated.h")

add_custom_command(
    OUTPUT ${ITEMS_HEADER}
    COMMAND ${EBTOOLS_EXE} generate items-header ${ITEMS_JSON} ${ITEMS_HEADER}
    DEPENDS ${ITEMS_JSON}
    COMMENT "Generating items_generated.h"
)
add_custom_target(items_header DEPENDS ${ITEMS_HEADER})

# earthbound_game needs items_generated.h (included via constants.h)
add_dependencies(earthbound_game items_header)

# Build-time struct info generator for state dump analysis
# Skip when cross-compiling — gen_struct_info is a host tool that can't run on the target.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(gen_struct_info tools/gen_struct_info.c)
    target_include_directories(gen_struct_info PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} include/ ${EMBED_GEN_DIR} ${EMBED_ASSET_HEADERS_DIR}
    )
    target_compile_definitions(gen_struct_info PRIVATE
        USA=1
        EB_VIEWPORT_WIDTH=${EB_VIEWPORT_WIDTH}
        EB_VIEWPORT_HEIGHT=${EB_VIEWPORT_HEIGHT}
        $<$<BOOL:${EB_ENABLE_AUDIO}>:EB_ENABLE_AUDIO=1>
    )
    add_dependencies(gen_struct_info items_header music_header)

    set(STRUCT_INFO_JSON "${CMAKE_CURRENT_BINARY_DIR}/struct_info.json")
    add_custom_command(
        OUTPUT ${STRUCT_INFO_JSON}
        COMMAND gen_struct_info > ${STRUCT_INFO_JSON}
        DEPENDS gen_struct_info ${ITEMS_HEADER} ${MUSIC_HEADER}
        COMMENT "Generating struct_info.json"
    )
    add_custom_target(struct_info ALL DEPENDS ${STRUCT_INFO_JSON})
endif()

# Sanitizer support (usage: cmake -DENABLE_ASAN=ON -DENABLE_UBSAN=ON ...)
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
    set(_SANITIZER_COMPILE_FLAGS -fsanitize=${_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    set(_SANITIZER_LINK_FLAGS -fsanitize=${_SANITIZER_FLAGS})

    target_compile_options(earthbound_game PRIVATE ${_SANITIZER_COMPILE_FLAGS})
    target_link_options(earthbound_game PRIVATE ${_SANITIZER_LINK_FLAGS})
    if(TARGET lakesnes_audio)
        target_compile_options(lakesnes_audio PRIVATE ${_SANITIZER_COMPILE_FLAGS})
        target_link_options(lakesnes_audio PRIVATE ${_SANITIZER_LINK_FLAGS})
    endif()

    if(TARGET lakesnes)
        target_compile_options(lakesnes PRIVATE ${_SANITIZER_COMPILE_FLAGS})
        target_link_options(lakesnes PRIVATE ${_SANITIZER_LINK_FLAGS})
    endif()
endif()

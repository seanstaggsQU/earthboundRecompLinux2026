"""The one place the "exclude these ROM byte ranges, use custom_dir instead"
list lives. Both the real build (src/CMakeLists.txt's EMBED_EXCLUDE_ARGS)
and the bundled `ebtools setup` helper (ebtools/cli/setup.py) need the exact
same list -- a mismatch here means a ROM-only build and a normal build
would disagree on what a given asset actually contains. If you change one,
change the other.
"""

DEFAULT_ASSET_EXCLUDES: list[str] = [
    "overworld_sprites/gfx/*",
    "US/overworld_sprites/gfx/*",
    "psianims/arrangements/*.arr.lzhal",
    "data/EEVENT*.bin",
    "data/EBATTLE*.bin",
    "data/EGOODS*.bin",
    "data/ESYSTEM.bin",
    "data/EGLOBAL.bin",
    "data/EHINT.bin",
    "data/ENEWS.bin",
    "data/ESHOP*.bin",
    "data/EEXPL*.bin",
    "data/EBGMESS.bin",
    "data/EDEBUG.bin",
    "data/KEYBOARD.bin",
    "data/DEBUG_TEXT.bin",
    "data/DOOR_SCRIPTS.bin",
    "data/UNKNOWN_EFA2FA.bin",
    "data/E0[1-9]*.bin",
    "data/E1[0-9]*.bin",
    "dialogue/addr_remap.bin",
    "data/compressed_text_data.bin",
    "data/compressed_text_ptrs.bin",
]

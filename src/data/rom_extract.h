#ifndef DATA_ROM_EXTRACT_H
#define DATA_ROM_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>

/* ROM discovery + a from-scratch pak builder, no ebtools/Python involved.
 *
 * Only compiled when EB_RUNTIME_ASSETS is defined.
 *
 * rom_extract_find_rom() is what main.c actually uses on a missing pak: it
 * locates a player's ROM, then hands it to the bundled `ebtools-setup`
 * standalone helper (the real extract/pack-all/pack-assets pipeline) to
 * build a correct, complete pak -- see ebtools/cli/setup.py. That helper
 * is authoritative for every asset, including ones that are a real
 * decode+recompile (dialogue text) or a JSON/PNG repack (sprites, town
 * maps, item/enemy tables, ...), not just a ROM byte range.
 *
 * rom_extract_build_pak()/rom_extract_scan_and_build_pak() below are a
 * pure-C, no-subprocess pak builder using rom_extract_table.c (generated
 * by `ebtools embed-registry --runtime`) -- correct for the subset of
 * assets that really are a plain ROM byte range (confirmed via
 * independent verification against ebtools' own decode), but it leaves
 * every asset that needs real repacking (dialogue, most graphics, most
 * data tables -- the majority of the game, by count) empty. Not used by
 * main.c's boot path for that reason. Kept for now as a tested, working
 * building block (e.g. its PSI arrangement rebundling) in case a future
 * no-subprocess path is worth revisiting -- not dead code to be confused
 * with a second, competing "authoritative" pak builder. */

typedef enum {
    EB_ROM_EXTRACT_OK = 0,
    EB_ROM_EXTRACT_IO_ERROR,     /* couldn't read the ROM file */
    EB_ROM_EXTRACT_TOO_SMALL,    /* file too small to be a real ROM */
    EB_ROM_EXTRACT_NOT_MATCHED,  /* checksum/title didn't match this game */
    EB_ROM_EXTRACT_WRITE_FAILED, /* couldn't write the output pak */
} EbRomExtractResult;

/* Slices rom_path per rom_extract_table[] and writes an EBPK-format pak to
 * out_path (creating parent directories as needed), written atomically via
 * a temp file + rename so a reader never sees a partial pak. */
EbRomExtractResult rom_extract_build_pak(const char *rom_path, const char *out_path);

/* Scans `dir` (non-recursive) for any .sfc/.smc file and tries each one
 * against rom_extract_build_pak() until one matches -- the ROM's own
 * checksum/title check (not the filename) decides what's a real match, so
 * this works no matter what the player named their ROM file. Returns
 * EB_ROM_EXTRACT_OK on the first match (pak written to out_path).
 * EB_ROM_EXTRACT_NOT_MATCHED if the directory had .sfc/.smc files but none
 * matched. EB_ROM_EXTRACT_IO_ERROR if the directory couldn't be scanned or
 * had no .sfc/.smc files at all -- callers should treat that the same as
 * "no ROM found" (there was nothing to try). */
EbRomExtractResult rom_extract_scan_and_build_pak(const char *dir, const char *out_path);

/* Scans `dir` (non-recursive) for any .sfc/.smc file that validates as a
 * real EarthBound ROM (checksum + title, not just the filename) and
 * copies its full path into out_path (out_cap bytes). Same matching logic
 * as rom_extract_scan_and_build_pak(), just without building anything --
 * for callers (see main.c) that hand the ROM off to an external tool
 * instead of building the pak directly in C. Returns true if a match was
 * found. */
bool rom_extract_find_rom(const char *dir, char *out_path, size_t out_cap);

#endif /* DATA_ROM_EXTRACT_H */

/*
 * ROM-only dialogue compiler: builds dialogue.bin from a donor ROM's raw
 * text blocks, matching what ebtools/cli/pack_all.py::_pack_dialogue()
 * produces from hand-edited dialogue YAML -- see
 * ~/.claude/plans/tranquil-launching-tome.md for the full design.
 */
#ifndef TEXT_COMPILE_H
#define TEXT_COMPILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Builds the flat dialogue blob from `rom` (a full, already-validated ROM
 * image of at least `rom_avail` bytes). On success, *out_buf is a
 * malloc'd buffer the caller owns (free() it) holding *out_size bytes,
 * and returns true. On failure, *out_buf and *out_size are left untouched
 * and this returns false -- callers should treat that as a hard error
 * (fail the whole pak build), not something to paper over: see
 * text_dialogue_source.py's doc comment for why an unmapped address
 * reference should never happen for this game's real dialogue data, and
 * why silently leaving a stale ROM address in the output would be worse
 * than failing loudly. */
bool text_compile_build_dialogue_blob(const uint8_t *rom, size_t rom_avail,
                                       uint8_t **out_buf, size_t *out_size);

#endif /* TEXT_COMPILE_H */

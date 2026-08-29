/*
 * ROM-only dialogue relocation: transforms one message's raw text
 * bytecode into a form the C runtime can actually read -- addresses
 * relocated to their new location, and compressed-text references
 * expanded to literal bytes (the runtime's own CC 0x15-0x17 expansion is
 * dormant/not wired up, see display_text.c) -- see
 * ~/.claude/plans/tranquil-launching-tome.md for the full design.
 *
 * This is NOT a port of decoder.py + compiler.py's full round trip. It's
 * a narrower, provably-equivalent walk for this one use case: given a
 * message's raw bytes (already known, from a checked-in label-offset
 * table -- see the plan doc for why block/label boundaries don't need
 * any runtime discovery), walk it exactly like the interpreter/
 * decoder.py does, copying bytes through unchanged except for the two
 * cases that need transformation. Everything else -- literal text
 * characters, unknown/unmapped opcodes -- is opaque bytes of known width
 * to copy through as-is: decoder.py's `text_table` lookup exists only to
 * turn those bytes into human-readable characters for YAML editing,
 * which this path never does, and text_table's byte range (0x50-0xAD,
 * confirmed empirically against earthbound.yml) never overlaps the
 * 0x00-0x1F control-code range anyway, so "not a known opcode" and "is a
 * text_table character" are the same "copy N bytes, unchanged" behavior
 * here regardless of which one it actually is.
 *
 * Two-call convention (measure, then write), same idiom as snprintf:
 * call once with out=NULL to learn the output size, allocate, then call
 * again with a real buffer. Both calls must use the same resolve_addr
 * callback and produce a consistent result -- see text_relocate_message's
 * own doc comment.
 */
#ifndef TEXT_RELOCATE_H
#define TEXT_RELOCATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Called once per relocatable address field encountered (a LABEL arg, a
 * JUMP_TABLE entry, or a STRING arg's select_script address), with the
 * original value already read from the input. Returns the value to
 * write instead. Most addresses are known label addresses and get
 * relocated; anything else (e.g. a sentinel like 0x000000 in an unused
 * menu-destination slot) should come back unchanged -- see compiler.py's
 * _resolve_label(), which only ever transforms a value that was
 * previously replaced with a string label name; everything else round-
 * trips as-is. */
typedef uint32_t (*TextRelocAddrResolver)(void *user, uint32_t original_value);

/* Transforms [data, data+length) (a single message's own span -- callers
 * must slice by label offset themselves, see the plan doc) into `out`.
 *
 * If `out` is NULL, writes nothing and just computes the required output
 * size into *out_size (a "measure" pass) -- use this to size a buffer
 * before the real write pass. If `out` is non-NULL, it must have room
 * for at least *out_size bytes (from a prior measure call against the
 * same input) and the transformed bytes are written there; *out_size is
 * still updated to the actual bytes written (should match the measure
 * pass exactly, since apart from allocation neither pass depends on
 * `out`).
 *
 * Returns false if the span is malformed in a way that would run an
 * argument past `length` (truncated opcode/string/jump table) -- callers
 * should treat that as a decode error, not something to guess through. */
bool text_relocate_message(const uint8_t *data, size_t length,
                            TextRelocAddrResolver resolve_addr, void *user,
                            uint8_t *out, size_t *out_size);

#endif /* TEXT_RELOCATE_H */

/*
 * ROM-only dialogue relocation: find every address field inside a span of
 * text bytecode that needs to be rewritten when the span is moved to a new
 * location (a new player's from-scratch dialogue.bin, built from a raw
 * ROM instead of the pre-migrated asset pipeline -- see
 * ~/.claude/plans/tranquil-launching-tome.md).
 *
 * This is NOT a port of decoder.py + compiler.py's full round trip. It's a
 * narrower, provably-equivalent operation for this one use case: given a
 * message's raw bytes (already known, from a checked-in label-offset
 * table -- see the plan doc for why block/label boundaries don't need any
 * runtime discovery), walk it exactly like the interpreter/decoder.py
 * does, but only to find the byte position of every relocatable address
 * field. Everything else -- literal text characters, compressed-text
 * expansion, unknown/unmapped opcodes -- is just opaque bytes of known
 * width to skip over unchanged: decoder.py's `text_table` lookup exists
 * only to turn those bytes into human-readable characters for YAML
 * editing, which this path never does, and text_table's byte range
 * (0x50-0xAD, confirmed empirically against earthbound.yml) never
 * overlaps the 0x00-0x1F control-code range anyway, so "not a known
 * opcode" and "is a text_table character" are the same "skip N bytes,
 * unchanged" behavior here regardless of which one it actually is.
 *
 * Once every field position is known, the caller can copy the span
 * verbatim and overwrite just those 4-byte fields with relocated
 * addresses -- byte-identical output to decoding then recompiling with
 * every argument value already an int (the only path this project's
 * ROM-only build ever takes; see compiler.py's dead symbolic-name-
 * resolution code for the case this deliberately does NOT replicate).
 */
#ifndef TEXT_RELOCATE_H
#define TEXT_RELOCATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Called once per relocatable address field found, with its byte offset
 * (relative to the start of the scanned span). The field is always
 * exactly 4 bytes, little-endian, holding either a raw SNES address
 * (0xC00000+) or an already-flat blob offset -- the caller decides how to
 * translate it; this scanner only locates it. */
typedef void (*TextRelocFieldCb)(void *user, size_t field_offset);

/* Scans exactly [data, data+length) -- callers must pass a single
 * message's own span (label_offset to the next label's offset, or block
 * end), not a whole multi-message block, since nothing here detects
 * message boundaries on its own (see the plan doc: those come from the
 * checked-in label-offset table, not from parsing).
 *
 * Returns false if the span is malformed in a way that would run an
 * argument past `length` (truncated opcode/string/jump table) -- callers
 * should treat that as a decode error, not something to guess through.
 * On success, `cb` has already been called for every relocatable field. */
bool text_scan_relocatable_fields(const uint8_t *data, size_t length,
                                   TextRelocFieldCb cb, void *user);

#endif /* TEXT_RELOCATE_H */

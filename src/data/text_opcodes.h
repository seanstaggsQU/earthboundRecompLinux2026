/*
 * Opcode table for the EarthBound text bytecode format, for the ROM-only
 * dialogue compiler (text_compile.c).
 *
 * Direct, faithful port of ebtools/text_dsl/opcodes.py -- same opcode list,
 * same byte sequences, same argument order/types. Keep the two in sync by
 * hand; there's no generation step for this one (it's hand-maintained
 * Python, ported by hand). Compressed text opcodes (0x15-0x17) are handled
 * separately by the decoder/compiler, same as in the Python version, and
 * are not listed here.
 *
 * Unlike opcodes.py, this table has no use for the "named" argument types
 * (ITEM, WINDOW, PARTY, MUSIC, SFX, SPRITE, MOVEMENT, STATUS_GROUP,
 * ENEMY_GROUP) resolving to symbolic string names -- the ROM-only path
 * never goes through YAML, so every argument value is always already a
 * plain integer. Each ArgType here still records its own *byte width*
 * (that's the only thing that actually matters for decode/recompile),
 * collapsing what opcodes.py splits into ~10 "named" types plus the
 * underlying U8/U16/U24/U32 types into just the widths themselves.
 */
#ifndef TEXT_OPCODES_H
#define TEXT_OPCODES_H

#include <stdint.h>

/* Argument type: only byte width matters here (see file doc comment) --
 * LABEL and JUMP_TABLE are broken out because they need address relocation
 * (LABEL is a plain 4-byte SNES/flat address; JUMP_TABLE is a 1-byte count
 * followed by that many 4-byte addresses) and STRING needs its own
 * variable-length, terminator-driven read/write. */
typedef enum {
    TEXT_ARG_U8,
    TEXT_ARG_U16,
    TEXT_ARG_U24,
    TEXT_ARG_U32,
    TEXT_ARG_LABEL,       /* 4-byte address, needs relocation */
    TEXT_ARG_JUMP_TABLE,  /* 1-byte count + N x 4-byte addresses, each needs relocation */
    TEXT_ARG_STRING,      /* raw bytes up to a 0x00/0x01/0x02 terminator; 0x01 is followed by a 4-byte label address */
} TextArgType;

typedef struct {
    const char *name;   /* arg name, for diagnostics only */
    TextArgType type;
} TextArgSpec;

#define TEXT_OP_MAX_ARGS 5

typedef struct {
    const char *yaml_name;         /* for diagnostics only */
    uint8_t bytes[2];              /* opcode byte sequence, 1 or 2 bytes */
    uint8_t byte_count;            /* 1 or 2 */
    uint8_t arg_count;
    TextArgSpec args[TEXT_OP_MAX_ARGS];
} TextOpcodeSpec;

/* Populated from text_opcodes.c's TEXT_OPCODES[] the first time it's
 * needed (text_opcodes_find_by_bytes/find_by_name build simple linear/hash
 * lookups over it) -- see text_opcodes.c. */
extern const TextOpcodeSpec TEXT_OPCODES[];
extern const int TEXT_OPCODE_COUNT;

/* Look up an opcode by its 1 or 2 byte sequence (byte_count tells you which).
 * Returns NULL if unknown (decoder falls back to treating it as raw/unknown
 * data, same as decoder.py's "op": "unknown" path). */
const TextOpcodeSpec *text_opcode_find_by_bytes(const uint8_t *bytes, int byte_count);

#endif /* TEXT_OPCODES_H */

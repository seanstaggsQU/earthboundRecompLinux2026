/*
 * See text_relocate.h for what this does and why it's not a literal
 * decoder.py+compiler.py port.
 */
#include "data/text_relocate.h"

/* Only compiled when EB_RUNTIME_ASSETS is defined -- see text_compile.c's
 * matching guard for why (this file includes the same runtime_generated
 * header, only on the include path in that build mode). */
#ifdef EB_RUNTIME_ASSETS

#include "data/text_opcodes.h"
#include "text_dialogue_source.h" /* compressed-text expansion table (runtime_generated) */
#include "include/binary.h"

static bool is_compressed_prefix(uint8_t b) {
    return b == 0x15 || b == 0x16 || b == 0x17;
}

/* Appends `n` bytes from `src` to the output, if writing (out != NULL).
 * Always advances *out_pos, so a measure pass (out == NULL) still
 * computes the right size. */
static void emit_bytes(uint8_t *out, size_t *out_pos, const uint8_t *src, size_t n) {
    if (out != NULL) {
        for (size_t i = 0; i < n; i++) out[*out_pos + i] = src[i];
    }
    *out_pos += n;
}

static void emit_u32le(uint8_t *out, size_t *out_pos, uint32_t v) {
    if (out != NULL) {
        write_u32_le(out + *out_pos, v);
    }
    *out_pos += 4;
}

/* Copies one argument's bytes from data[*pos] to the output, resolving
 * LABEL/JUMP_TABLE/STRING-select_script address fields via resolve_addr.
 * Advances *pos (input) and *out_pos (output, only if out != NULL). */
static bool relocate_arg(const uint8_t *data, size_t length, size_t *pos,
                          TextArgType type, TextRelocAddrResolver resolve_addr, void *user,
                          uint8_t *out, size_t *out_pos) {
    size_t remaining = length - *pos;
    switch (type) {
    case TEXT_ARG_U8:
        if (remaining < 1) return false;
        emit_bytes(out, out_pos, data + *pos, 1);
        *pos += 1;
        return true;
    case TEXT_ARG_U16:
        if (remaining < 2) return false;
        emit_bytes(out, out_pos, data + *pos, 2);
        *pos += 2;
        return true;
    case TEXT_ARG_U24:
        if (remaining < 3) return false;
        emit_bytes(out, out_pos, data + *pos, 3);
        *pos += 3;
        return true;
    case TEXT_ARG_U32:
        if (remaining < 4) return false;
        emit_bytes(out, out_pos, data + *pos, 4);
        *pos += 4;
        return true;
    case TEXT_ARG_LABEL: {
        if (remaining < 4) return false;
        uint32_t original = read_u32_le(data + *pos);
        uint32_t resolved = resolve_addr(user, original);
        emit_u32le(out, out_pos, resolved);
        *pos += 4;
        return true;
    }
    case TEXT_ARG_JUMP_TABLE: {
        if (remaining < 1) return false;
        uint8_t count = data[*pos];
        emit_bytes(out, out_pos, data + *pos, 1);
        *pos += 1;
        for (uint8_t i = 0; i < count; i++) {
            if (length - *pos < 4) return false;
            uint32_t original = read_u32_le(data + *pos);
            uint32_t resolved = resolve_addr(user, original);
            emit_u32le(out, out_pos, resolved);
            *pos += 4;
        }
        return true;
    }
    case TEXT_ARG_STRING: {
        /* Raw bytes up to a 0x00 (end), 0x01 (select_script, followed by
         * a 4-byte label address), or 0x02 (store) terminator -- see
         * decoder.py's _read_string_arg. */
        while (*pos < length) {
            uint8_t b = data[*pos];
            if (b == 0x00 || b == 0x02) {
                emit_bytes(out, out_pos, data + *pos, 1);
                *pos += 1;
                return true;
            } else if (b == 0x01) {
                emit_bytes(out, out_pos, data + *pos, 1);
                *pos += 1;
                if (length - *pos < 4) return false;
                uint32_t original = read_u32_le(data + *pos);
                uint32_t resolved = resolve_addr(user, original);
                emit_u32le(out, out_pos, resolved);
                *pos += 4;
                return true;
            }
            emit_bytes(out, out_pos, data + *pos, 1);
            *pos += 1;
        }
        return false; /* ran out of data without a terminator */
    }
    default:
        return false;
    }
}

bool text_relocate_message(const uint8_t *data, size_t length,
                            TextRelocAddrResolver resolve_addr, void *user,
                            uint8_t *out, size_t *out_size) {
    size_t pos = 0;
    size_t out_pos = 0;

    while (pos < length) {
        uint8_t b = data[pos];

        /* Compressed text: prefix + 1 index byte in, expansion bytes out. */
        if (is_compressed_prefix(b)) {
            if (length - pos < 2) return false;
            uint16_t index = (uint16_t)((b - 0x15) * 256 + data[pos + 1]);
            pos += 2;
            if (index < TEXT_COMPRESSED_EXPANSION_COUNT) {
                const TextCompressedExpansion *exp = &text_compressed_expansions[index];
                emit_bytes(out, &out_pos, text_compressed_expansion_bytes + exp->byte_offset, exp->length);
            }
            /* index out of range: matches decoder.py's "not in compressed_text
             * dict, silently skip (no output)" -- shouldn't happen (all 768
             * slots are populated for this game), but stay consistent with
             * the reference behavior rather than inventing an error case. */
            continue;
        }

        /* Two-byte opcode prefix range. */
        if (b >= 0x18 && b <= 0x1F) {
            if (length - pos < 2) return false;
            uint8_t bytes2[2] = { b, data[pos + 1] };
            const TextOpcodeSpec *op = text_opcode_find_by_bytes(bytes2, 2);
            emit_bytes(out, &out_pos, data + pos, 2);
            pos += 2;
            if (op == NULL) continue; /* unknown 2-byte opcode: raw passthrough, no args */
            for (uint8_t i = 0; i < op->arg_count; i++) {
                if (!relocate_arg(data, length, &pos, op->args[i].type, resolve_addr, user, out, &out_pos))
                    return false;
            }
            continue;
        }

        /* Single-byte opcode (primary codes 0x00-0x14). Anything else --
         * a literal text character or an unrecognized byte -- is opaque,
         * 1 byte, copied through unchanged. */
        uint8_t bytes1[1] = { b };
        const TextOpcodeSpec *op = text_opcode_find_by_bytes(bytes1, 1);
        emit_bytes(out, &out_pos, data + pos, 1);
        pos += 1;
        if (op == NULL) continue;
        for (uint8_t i = 0; i < op->arg_count; i++) {
            if (!relocate_arg(data, length, &pos, op->args[i].type, resolve_addr, user, out, &out_pos))
                return false;
        }
    }

    *out_size = out_pos;
    return true;
}

static uint32_t noop_resolve_addr(void *user, uint32_t original_value) {
    (void)user;
    return original_value;
}

bool text_find_message_end(const uint8_t *data, size_t max_length, size_t *out_length) {
    size_t pos = 0;
    size_t discard_out_pos; /* relocate_arg still wants somewhere to track a
                              * would-be output position; out=NULL means it
                              * never actually writes through it, but it must
                              * NOT be aliased to `pos` itself (relocate_arg
                              * advances both independently -- aliasing them
                              * would double-advance `pos`). Reset it to 0
                              * before each call since only the *delta*
                              * relocate_arg would apply matters, not any
                              * absolute value, and nothing here ever reads it. */

    while (pos < max_length) {
        uint8_t b = data[pos];

        if (is_compressed_prefix(b)) {
            if (max_length - pos < 2) return false;
            pos += 2;
            continue;
        }

        if (b >= 0x18 && b <= 0x1F) {
            if (max_length - pos < 2) return false;
            uint8_t bytes2[2] = { b, data[pos + 1] };
            const TextOpcodeSpec *op = text_opcode_find_by_bytes(bytes2, 2);
            pos += 2;
            if (op == NULL) continue;
            for (uint8_t i = 0; i < op->arg_count; i++) {
                discard_out_pos = 0;
                if (!relocate_arg(data, max_length, &pos, op->args[i].type, noop_resolve_addr, NULL, NULL, &discard_out_pos))
                    return false;
            }
            continue;
        }

        /* end_block (0x02): single byte, no args -- this is where a
         * standalone message (item/PSI/battle-action help text, with no
         * pre-known end offset the way dialogue.bin's labeled messages
         * have) actually ends. Matches decoder.py's own primary-opcode
         * table (OpcodeSpec("end_block", (0x02,))). */
        if (b == 0x02) {
            *out_length = pos + 1;
            return true;
        }

        uint8_t bytes1[1] = { b };
        const TextOpcodeSpec *op = text_opcode_find_by_bytes(bytes1, 1);
        pos += 1;
        if (op == NULL) continue;
        for (uint8_t i = 0; i < op->arg_count; i++) {
            discard_out_pos = 0;
            if (!relocate_arg(data, max_length, &pos, op->args[i].type, noop_resolve_addr, NULL, NULL, &discard_out_pos))
                return false;
        }
    }

    return false; /* ran out of data without finding end_block */
}

#endif /* EB_RUNTIME_ASSETS */

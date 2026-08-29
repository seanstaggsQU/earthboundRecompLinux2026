/*
 * See text_relocate.h for what this does and why it's not a literal
 * decoder.py+compiler.py port.
 */
#include "data/text_relocate.h"
#include "data/text_opcodes.h"

/* Compressed-text prefix bytes -- decoder.py's _COMPRESSED_TEXT_PREFIXES.
 * Each is a 2-byte unit: prefix + index byte. Nothing inside is
 * relocatable (compressed strings never contain addresses). */
static bool is_compressed_prefix(uint8_t b) {
    return b == 0x15 || b == 0x16 || b == 0x17;
}

/* Consumes one argument's bytes starting at data[*pos], advancing *pos by
 * its width, and reporting any relocatable address field via cb. Returns
 * false if the argument would run past `length`. */
static bool consume_arg(const uint8_t *data, size_t length, size_t *pos,
                         TextArgType type, TextRelocFieldCb cb, void *user) {
    size_t remaining = length - *pos;
    switch (type) {
    case TEXT_ARG_U8:
        if (remaining < 1) return false;
        *pos += 1;
        return true;
    case TEXT_ARG_U16:
        if (remaining < 2) return false;
        *pos += 2;
        return true;
    case TEXT_ARG_U24:
        if (remaining < 3) return false;
        *pos += 3;
        return true;
    case TEXT_ARG_U32:
        if (remaining < 4) return false;
        *pos += 4;
        return true;
    case TEXT_ARG_LABEL:
        if (remaining < 4) return false;
        cb(user, *pos);
        *pos += 4;
        return true;
    case TEXT_ARG_JUMP_TABLE: {
        if (remaining < 1) return false;
        uint8_t count = data[*pos];
        *pos += 1;
        for (uint8_t i = 0; i < count; i++) {
            if (length - *pos < 4) return false;
            cb(user, *pos);
            *pos += 4;
        }
        return true;
    }
    case TEXT_ARG_STRING: {
        /* Raw bytes up to a 0x00 (end), 0x01 (select_script, followed by
         * a 4-byte label address), or 0x02 (store) terminator -- see
         * decoder.py's _read_string_arg. Ran-out-of-data without hitting
         * one is decoder.py's own fallback ("terminator": "end"), not an
         * error there, but for a well-formed ROM message this shouldn't
         * happen; treat it as truncation here same as everywhere else. */
        while (*pos < length) {
            uint8_t b = data[*pos];
            if (b == 0x00) {
                *pos += 1;
                return true;
            } else if (b == 0x01) {
                *pos += 1;
                if (length - *pos < 4) return false;
                cb(user, *pos);
                *pos += 4;
                return true;
            } else if (b == 0x02) {
                *pos += 1;
                return true;
            }
            *pos += 1;
        }
        return false; /* ran out of data without a terminator */
    }
    default:
        return false;
    }
}

bool text_scan_relocatable_fields(const uint8_t *data, size_t length,
                                   TextRelocFieldCb cb, void *user) {
    size_t pos = 0;

    while (pos < length) {
        uint8_t b = data[pos];

        /* Compressed text: prefix + 1 index byte, nothing relocatable. */
        if (is_compressed_prefix(b)) {
            if (length - pos < 2) return false;
            pos += 2;
            continue;
        }

        /* Two-byte opcode prefix range. */
        if (b >= 0x18 && b <= 0x1F) {
            if (length - pos < 2) return false;
            uint8_t bytes2[2] = { b, data[pos + 1] };
            const TextOpcodeSpec *op = text_opcode_find_by_bytes(bytes2, 2);
            pos += 2;
            if (op == NULL) continue; /* unknown 2-byte opcode: raw passthrough, no args */
            for (uint8_t i = 0; i < op->arg_count; i++) {
                if (!consume_arg(data, length, &pos, op->args[i].type, cb, user))
                    return false;
            }
            continue;
        }

        /* Single-byte opcode (primary codes 0x00-0x14). Anything else --
         * a literal text character or an unrecognized byte -- is opaque,
         * 1 byte, unchanged (see text_relocate.h's doc comment for why
         * that's correct without needing text_table at all). */
        uint8_t bytes1[1] = { b };
        const TextOpcodeSpec *op = text_opcode_find_by_bytes(bytes1, 1);
        pos += 1;
        if (op == NULL) continue;
        for (uint8_t i = 0; i < op->arg_count; i++) {
            if (!consume_arg(data, length, &pos, op->args[i].type, cb, user))
                return false;
        }
    }

    return true;
}

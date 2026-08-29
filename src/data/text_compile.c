/*
 * See text_compile.h. Builds dialogue.bin from a donor ROM at runtime,
 * matching pack_all.py::_pack_dialogue()'s output for the same ROM (up
 * to the compressed-text-expansion difference documented in
 * text_dialogue_source.py's generator doc comment).
 *
 * Two-phase build, same shape as _pack_dialogue() itself:
 *   Phase 1: walk every block's labels in order (text_dialogue_blocks[],
 *     already sorted alphabetically by block name to match
 *     sorted(dialogue_dir.glob("*.yml")) -- see text_dialogue_source.py),
 *     measuring each message's *output* size (text_relocate_message()'s
 *     measure pass -- NOT the same as its raw ROM byte-span length,
 *     since compressed-text expansion can make it longer) and assigning
 *     each a flat offset (DIALOGUE_BLOB_BASE + running byte position).
 *     Address resolution doesn't affect size (a LABEL field is always 4
 *     bytes either way), so this pass uses a dummy resolver that just
 *     returns its input unchanged -- only compressed-text expansion
 *     matters for sizing.
 *   Phase 2: for every message, call text_relocate_message() again, this
 *     time with the real resolver (backed by the address remap table
 *     built during phase 1) and a real output buffer, writing directly
 *     into the growing blob.
 */
#include "data/text_compile.h"

/* Only compiled when EB_RUNTIME_ASSETS is defined -- this file's whole
 * purpose is building dialogue.bin from a raw ROM at runtime, same
 * "no-op translation unit otherwise" convention as rom_extract.c (which
 * this feeds into), since it includes runtime_generated/
 * text_dialogue_source.h, only added to the include path in that build
 * mode. */
#ifdef EB_RUNTIME_ASSETS

#include "text_dialogue_source.h" /* runtime_generated */
#include "data/text_relocate.h"
#include <stdlib.h>
#include <string.h>

#define DIALOGUE_BLOB_BASE 0x100000u
#define SNES_TEXT_BASE 0xC00000u

typedef struct {
    uint32_t snes_addr;
    uint32_t flat_offset;
} AddrRemapEntry;

static uint32_t addr_remap_lookup(const AddrRemapEntry *remap, size_t count,
                                   uint32_t snes_addr, bool *found) {
    /* Linear scan: this runs once per relocatable field (~7310 for the
     * real game) against ~6074 entries -- tens of millions of comparisons
     * worst case, comfortably fast for a one-time asset-pack build. Not
     * worth a sorted/binary-search or hash structure unless this turns
     * out to actually be slow in practice. */
    for (size_t i = 0; i < count; i++) {
        if (remap[i].snes_addr == snes_addr) {
            *found = true;
            return remap[i].flat_offset;
        }
    }
    *found = false;
    return 0;
}

/* Phase 1's resolver: address resolution never changes a field's byte
 * width, so sizing doesn't need real answers, just something to call. */
static uint32_t dummy_resolve_addr(void *user, uint32_t original_value) {
    (void)user;
    return original_value;
}

typedef struct {
    const AddrRemapEntry *remap;
    size_t remap_count;
} RealResolveCtx;

static uint32_t real_resolve_addr(void *user, uint32_t original_value) {
    RealResolveCtx *ctx = (RealResolveCtx *)user;
    bool found = false;
    /* Matches compiler.py's _resolve_label() exactly: a value only ever
     * gets relocated if it was a KNOWN label address, because the real
     * pipeline's relocation happens indirectly (migrate_text.py replaces
     * a known address with a string label name; compile_text_block()
     * later resolves that name back to a, possibly new, offset). A raw
     * int address that was never a known label -- a sentinel like
     * 0x000000 in an unused menu-destination slot, or any other
     * non-label value that happens to live in a LABEL-typed field --
     * never goes through that substitution and is written back
     * completely unchanged by _resolve_label() (`return value` for a
     * non-string). So "not found" here isn't an error, it's the correct,
     * faithful behavior: leave it alone. */
    uint32_t resolved = addr_remap_lookup(ctx->remap, ctx->remap_count, original_value, &found);
    return found ? resolved : original_value;
}

static uint16_t message_end_offset(const TextDialogueBlockSource *block, uint32_t li) {
    return (li + 1 < block->label_count)
               ? text_dialogue_labels[block->label_start_index + li + 1]
               : (uint16_t)block->rom_size;
}

bool text_compile_build_dialogue_blob(const uint8_t *rom, size_t rom_avail,
                                       uint8_t **out_buf, size_t *out_size) {
    /* --- Phase 1: measure every message's output size, assign flat
     * offsets, build the address remap table --- */
    AddrRemapEntry *remap = malloc(sizeof(AddrRemapEntry) * (size_t)TEXT_DIALOGUE_LABEL_COUNT);
    if (!remap) return false;

    size_t remap_count = 0;
    uint32_t running_pos = 0;

    for (int b = 0; b < TEXT_DIALOGUE_BLOCK_COUNT; b++) {
        const TextDialogueBlockSource *block = &text_dialogue_blocks[b];
        if ((uint64_t)block->rom_offset + (uint64_t)block->rom_size > (uint64_t)rom_avail) {
            free(remap);
            return false; /* block's ROM range doesn't fit this ROM -- malformed donor ROM */
        }
        uint32_t block_snes_base = SNES_TEXT_BASE + block->rom_offset;
        const uint8_t *block_data = rom + block->rom_offset;

        for (uint32_t li = 0; li < block->label_count; li++) {
            uint16_t label_off = text_dialogue_labels[block->label_start_index + li];
            uint16_t next_off = message_end_offset(block, li);
            if (next_off < label_off || next_off > block->rom_size) {
                free(remap);
                return false; /* malformed generated table -- shouldn't happen */
            }
            uint32_t raw_len = (uint32_t)(next_off - label_off);

            size_t measured_size = 0;
            if (!text_relocate_message(block_data + label_off, raw_len,
                                        dummy_resolve_addr, NULL, NULL, &measured_size)) {
                free(remap);
                return false;
            }

            remap[remap_count].snes_addr = block_snes_base + label_off;
            remap[remap_count].flat_offset = DIALOGUE_BLOB_BASE + running_pos;
            remap_count++;

            running_pos += (uint32_t)measured_size;
        }
    }

    size_t total_size = running_pos;
    uint8_t *blob = malloc(total_size ? total_size : 1);
    if (!blob) {
        free(remap);
        return false;
    }

    /* --- Phase 2: write each message's relocated+expanded bytes --- */
    RealResolveCtx resolve_ctx = { remap, remap_count };
    uint32_t write_pos = 0;
    bool ok = true;

    for (int b = 0; b < TEXT_DIALOGUE_BLOCK_COUNT && ok; b++) {
        const TextDialogueBlockSource *block = &text_dialogue_blocks[b];
        const uint8_t *block_data = rom + block->rom_offset;

        for (uint32_t li = 0; li < block->label_count && ok; li++) {
            uint16_t label_off = text_dialogue_labels[block->label_start_index + li];
            uint16_t next_off = message_end_offset(block, li);
            uint32_t raw_len = (uint32_t)(next_off - label_off);

            size_t written_size = 0;
            if (!text_relocate_message(block_data + label_off, raw_len,
                                        real_resolve_addr, &resolve_ctx,
                                        blob + write_pos, &written_size)) {
                ok = false;
                break;
            }

            write_pos += (uint32_t)written_size;
        }
    }

    free(remap);

    if (!ok || write_pos != total_size) {
        free(blob);
        return false;
    }

    *out_buf = blob;
    *out_size = total_size;
    return true;
}

#endif /* EB_RUNTIME_ASSETS */

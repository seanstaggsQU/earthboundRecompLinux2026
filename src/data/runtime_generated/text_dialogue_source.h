/* GENERATED FILE -- do not hand-edit. See ebtools/parsers/text_dialogue_source.py
 * and `ebtools generate text-dialogue-source`. */

#ifndef TEXT_DIALOGUE_SOURCE_H
#define TEXT_DIALOGUE_SOURCE_H

#include <stdint.h>

typedef struct {
    uint32_t rom_offset;      /* raw ROM file byte offset; SNES base addr = rom_offset + 0xC00000 */
    uint32_t rom_size;
    uint32_t label_start_index; /* index into text_dialogue_labels[] */
    uint16_t label_count;
} TextDialogueBlockSource;

#define TEXT_DIALOGUE_BLOCK_COUNT 61
extern const TextDialogueBlockSource text_dialogue_blocks[TEXT_DIALOGUE_BLOCK_COUNT];

/* Byte offset (within its own block) of every known message start,
 * sorted ascending within each block, concatenated in block order.
 * Index a block's own slice via its label_start_index/label_count. */
#define TEXT_DIALOGUE_LABEL_COUNT 6074
extern const uint16_t text_dialogue_labels[TEXT_DIALOGUE_LABEL_COUNT];

/* CC 0x15-0x17 compressed-text expansion table -- see this file's
 * generator doc comment for why the C runtime needs pre-expanded
 * bytes instead of the original compact 2-byte references. Index:
 * byte0 in [0x15,0x17], byte1 the low byte -> (byte0-0x15)*256+byte1. */
typedef struct {
    uint16_t byte_offset; /* into text_compressed_expansion_bytes[] */
    uint8_t length;
} TextCompressedExpansion;

#define TEXT_COMPRESSED_EXPANSION_COUNT 768
extern const TextCompressedExpansion text_compressed_expansions[TEXT_COMPRESSED_EXPANSION_COUNT];
extern const uint8_t text_compressed_expansion_bytes[3776];

#endif /* TEXT_DIALOGUE_SOURCE_H */

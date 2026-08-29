"""Generate the raw ebtxt block + label-offset source table for the ROM-only
dialogue compiler (src/data/text_compile.c).

Every text block referenced by a dialogue message is a contiguous ROM byte
range (same one the human-editable dialogue YAML was originally decoded
from, see ebtools/cli/migrate_text.py). This table gives the C runtime
everything it needs to re-derive that same decoding on a brand-new
player's own ROM, with no YAML/Python involved at that point:

  - Each block's raw (rom_offset, rom_size) -- same convention as
    rom_extract_table.c (entry.offset IS the ROM file byte offset; the
    block's base SNES address is rom_offset + 0xC00000).
  - Each block's sorted list of message-start byte offsets (within the
    block), taken directly from earthbound.yml's renameLabels. Verified
    empirically (2026-08-28) that every label present in the checked-in
    src/assets/dialogue/*.yml files already exists in renameLabels for
    every block in this ROM -- migrate_text.py's runtime auto-label
    fallback (for a jump/call target that lands mid-message, not at a
    curated label) never actually fires for this game's data, so this
    table doesn't need to replicate that discovery logic. text_compile.c
    should still fail loudly (not silently misdecode) if it ever
    encounters a LABEL/JUMP_TABLE reference outside every known label's
    coverage, as a safety net for that assumption -- not something to
    silently paper over if it's ever wrong for different ROM data.
"""

from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ebtools.config import DumpDoc


def generate_text_dialogue_source(doc: "DumpDoc", output_dir: Path) -> None:
    """Write text_dialogue_source.h/.c into output_dir (src/data/runtime_generated/).

    Also bakes in the compressed-text (CC 0x15-0x17) expansion table, as
    raw already-encoded bytes -- the C runtime's own compressed-text
    expansion is dormant/not wired up (see display_text.c), so the
    original ROM's size reduction from this scheme can't survive a
    ROM-only build's dialogue.bin as-is: every compressed-text reference
    has to be expanded to literal bytes at build time instead, exactly
    like the already-shipped migrated dialogue.bin does (decoder.py
    permanently expands compressed text into literal characters during
    migration; there's no re-compression step in compile_text_block()).
    Pre-encoding each of the 768 expansion strings to bytes here (via
    textTable, the same table decoder.py/compiler.py use) means the C
    runtime never needs textTable/reverse_text_table for anything -- it
    just copies already-correct bytes out of this table.
    """
    reverse_text_table: dict[str, int] = {v: k for k, v in doc.textTable.items()}

    def encode_text(s: str) -> bytes:
        out = bytearray()
        for ch in s:
            if ch not in reverse_text_table:
                raise ValueError(f"character {ch!r} in compressed-text string {s!r} not in textTable")
            out.append(reverse_text_table[ch])
        return bytes(out)

    # pack_all.py's _pack_dialogue() concatenates blocks in
    # sorted(dialogue_dir.glob("*.yml")) order -- i.e. alphabetical by
    # block name (== filename, one block per YAML file) -- NOT
    # dumpEntries' order. Match that exactly: this table's block order is
    # what determines every label's flat offset, and the plan's
    # verification bar is byte-identical output against the Python
    # pipeline, so the concatenation order has to match, not just be
    # internally self-consistent.
    ebtxt_entries = sorted((e for e in doc.dumpEntries if e.extension == "ebtxt"), key=lambda e: e.name)
    # bin-extension entries share the same (offset, size) as their ebtxt
    # sibling for the same name -- use whichever is easiest; bin matches
    # the convention every other raw-byte-range table already uses.
    bin_by_name = {e.name: e for e in doc.dumpEntries if e.extension == "bin"}

    blocks: list[tuple[str, int, int, list[int]]] = []  # name, rom_offset, rom_size, label_offsets
    for entry in ebtxt_entries:
        bin_entry = bin_by_name.get(entry.name)
        if bin_entry is None:
            print(f"WARNING: no .bin sibling for ebtxt block {entry.name!r}, skipping")
            continue
        labels = doc.renameLabels.get(entry.name, {})
        if not labels:
            continue
        label_offsets = sorted(labels.keys())
        blocks.append((entry.name, bin_entry.offset, bin_entry.size, label_offsets))

    header = (
        "/* GENERATED FILE -- do not hand-edit. See ebtools/parsers/text_dialogue_source.py\n"
        " * and `ebtools generate text-dialogue-source`. */\n"
    )

    hdr_lines = [
        header,
        "#ifndef TEXT_DIALOGUE_SOURCE_H",
        "#define TEXT_DIALOGUE_SOURCE_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    uint32_t rom_offset;      /* raw ROM file byte offset; SNES base addr = rom_offset + 0xC00000 */",
        "    uint32_t rom_size;",
        "    uint32_t label_start_index; /* index into text_dialogue_labels[] */",
        "    uint16_t label_count;",
        "} TextDialogueBlockSource;",
        "",
        f"#define TEXT_DIALOGUE_BLOCK_COUNT {len(blocks)}",
        "extern const TextDialogueBlockSource text_dialogue_blocks[TEXT_DIALOGUE_BLOCK_COUNT];",
        "",
        "/* Byte offset (within its own block) of every known message start,",
        " * sorted ascending within each block, concatenated in block order.",
        " * Index a block's own slice via its label_start_index/label_count. */",
    ]
    total_labels = sum(len(b[3]) for b in blocks)
    hdr_lines += [
        f"#define TEXT_DIALOGUE_LABEL_COUNT {total_labels}",
        "extern const uint16_t text_dialogue_labels[TEXT_DIALOGUE_LABEL_COUNT];",
        "",
    ]

    expansions = [encode_text(s) for s in doc.compressedTextStrings]
    total_expansion_bytes = sum(len(e) for e in expansions)
    hdr_lines += [
        "/* CC 0x15-0x17 compressed-text expansion table -- see this file's",
        " * generator doc comment for why the C runtime needs pre-expanded",
        " * bytes instead of the original compact 2-byte references. Index:",
        " * byte0 in [0x15,0x17], byte1 the low byte -> (byte0-0x15)*256+byte1. */",
        "typedef struct {",
        "    uint16_t byte_offset; /* into text_compressed_expansion_bytes[] */",
        "    uint8_t length;",
        "} TextCompressedExpansion;",
        "",
        f"#define TEXT_COMPRESSED_EXPANSION_COUNT {len(expansions)}",
        "extern const TextCompressedExpansion text_compressed_expansions[TEXT_COMPRESSED_EXPANSION_COUNT];",
        f"extern const uint8_t text_compressed_expansion_bytes[{total_expansion_bytes}];",
        "",
        "#endif /* TEXT_DIALOGUE_SOURCE_H */",
        "",
    ]
    (output_dir / "text_dialogue_source.h").write_text("\n".join(hdr_lines))

    src_lines = [
        header,
        '#include "text_dialogue_source.h"',
        "",
        "const TextDialogueBlockSource text_dialogue_blocks[TEXT_DIALOGUE_BLOCK_COUNT] = {",
    ]
    running_index = 0
    for name, rom_offset, rom_size, label_offsets in blocks:
        src_lines.append(
            f"    {{ {rom_offset}u, {rom_size}u, {running_index}u, {len(label_offsets)}u }}, /* {name} */"
        )
        running_index += len(label_offsets)
    src_lines.append("};")
    src_lines.append("")

    src_lines.append("const uint16_t text_dialogue_labels[TEXT_DIALOGUE_LABEL_COUNT] = {")
    for name, _rom_offset, _rom_size, label_offsets in blocks:
        joined = ", ".join(str(o) for o in label_offsets)
        src_lines.append(f"    {joined}, /* {name} ({len(label_offsets)} labels) */")
    src_lines.append("};")
    src_lines.append("")

    src_lines.append("const TextCompressedExpansion text_compressed_expansions[TEXT_COMPRESSED_EXPANSION_COUNT] = {")
    running_byte = 0
    for i, e in enumerate(expansions):
        src_lines.append(f"    {{ {running_byte}u, {len(e)}u }},")
        running_byte += len(e)
    src_lines.append("};")
    src_lines.append("")

    all_bytes = b"".join(expansions)
    src_lines.append(f"const uint8_t text_compressed_expansion_bytes[{total_expansion_bytes}] = {{")
    # 32 bytes per line, readable without being one giant line.
    for i in range(0, len(all_bytes), 32):
        chunk = all_bytes[i : i + 32]
        src_lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    src_lines.append("};")
    src_lines.append("")

    (output_dir / "text_dialogue_source.c").write_text("\n".join(src_lines))

    print(
        f"Generated text_dialogue_source.h/.c: {len(blocks)} blocks, {total_labels} labels, "
        f"{len(expansions)} compressed-text expansions ({total_expansion_bytes} bytes) in {output_dir}"
    )

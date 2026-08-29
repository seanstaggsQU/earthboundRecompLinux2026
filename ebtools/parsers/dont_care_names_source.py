"""Generates dont_care_names_source.h/.c: the "Don't Care" naming-screen
name pool (custom_assets/dont_care_names.json) as plain ASCII C string
literals, for the ROM-only build path (src/data/rom_extract.c).

This pool isn't a raw ROM byte range -- see rom_extract_table.c's {0,0}
gap for ASSET_US_DATA_DONT_CARE_NAMES_BIN -- it's this port's own custom
overlay on top of (in some slots, a full replacement of) the vanilla name
list, see custom_assets/dont_care_names.json itself and
ebtools/parsers/simple_tables.py's pack_dont_care_names() (the Python-side
packer this mirrors, used by the embedded-assets build). Unlike
dialogue.bin's compiler, this data needs no EB-text encoding done in
Python at all: the strings are plain ASCII, and the C runtime already has
ascii_to_eb_char() (text.c, the same encoder the naming screen itself uses
for typed names) to do that encoding at extraction time -- so this
generator just carries the JSON's strings through as C string literals,
in DONT_CARE_CATEGORIES order (simple_tables.py), and the C side
(dont_care_names_build(), rom_extract.c) does the actual encoding.
"""

import json
from pathlib import Path

# Mirrors ebtools/parsers/simple_tables.py's DONT_CARE_CATEGORIES exactly
# (json_key, max_usable_length) -- order matters, it's the on-disk order
# both the embedded-assets Python packer and this ROM-only C path must
# agree on.
DONT_CARE_CATEGORIES = [
    ("ness", 5),
    ("paula", 5),
    ("jeff", 5),
    ("poo", 5),
    ("pet", 6),
    ("food", 6),
    ("thing", 6),
]
DONT_CARE_PER_CATEGORY = 7


def _c_string_literal(s: str) -> str:
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def generate_dont_care_names_source(json_path: Path, output_dir: Path) -> None:
    names: dict[str, list[str]] = json.loads(json_path.read_text(encoding="utf-8"))

    for key, max_len in DONT_CARE_CATEGORIES:
        entries = names.get(key, [])
        if len(entries) != DONT_CARE_PER_CATEGORY:
            raise ValueError(
                f"dont_care_names.json: category '{key}' has {len(entries)} entries, "
                f"expected {DONT_CARE_PER_CATEGORY}"
            )
        for name in entries:
            if len(name) > max_len:
                raise ValueError(
                    f"dont_care_names.json: '{name}' in category '{key}' is {len(name)} "
                    f"chars, but the naming screen only uses the first {max_len} of this "
                    f"category -- it would be silently truncated in-game. Shorten it."
                )

    header_path = output_dir / "dont_care_names_source.h"
    source_path = output_dir / "dont_care_names_source.c"

    header_path.write_text(
        "/* GENERATED FILE -- do not hand-edit. See "
        "ebtools/parsers/dont_care_names_source.py and "
        "`ebtools generate dont-care-names-source`. */\n"
        "\n"
        "#ifndef DONT_CARE_NAMES_SOURCE_H\n"
        "#define DONT_CARE_NAMES_SOURCE_H\n"
        "\n"
        "#define DONT_CARE_CATEGORY_COUNT 7\n"
        "#define DONT_CARE_PER_CATEGORY 7\n"
        "\n"
        "/* DONT_CARE_CATEGORY_COUNT arrays of DONT_CARE_PER_CATEGORY plain\n"
        " * ASCII names each, in custom_assets/dont_care_names.json's own\n"
        " * category order (ness, paula, jeff, poo, pet, food, thing). */\n"
        "extern const char *const dont_care_names_source"
        "[DONT_CARE_CATEGORY_COUNT][DONT_CARE_PER_CATEGORY];\n"
        "\n"
        "#endif /* DONT_CARE_NAMES_SOURCE_H */\n",
        encoding="utf-8",
    )

    lines = [
        "/* GENERATED FILE -- do not hand-edit. See "
        "ebtools/parsers/dont_care_names_source.py and "
        "`ebtools generate dont-care-names-source`. */",
        "",
        '#include "dont_care_names_source.h"',
        "",
        "const char *const dont_care_names_source[DONT_CARE_CATEGORY_COUNT][DONT_CARE_PER_CATEGORY] = {",
    ]
    for key, _max_len in DONT_CARE_CATEGORIES:
        entries = names[key]
        row = ", ".join(_c_string_literal(e) for e in entries)
        lines.append(f"    /* {key} */ {{ {row} }},")
    lines.append("};")
    lines.append("")
    source_path.write_text("\n".join(lines), encoding="utf-8")

"""The ``embed-registry`` command: generate C source for embedded binary assets."""

import fnmatch
import hashlib
import re
import struct
import sys
import textwrap
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Annotated

from cyclopts import Parameter

# Locale prefixes in priority order.
_LOCALE_PREFIXES = ("US/", "JP/")

# .pak file format constants (see docs/assets.md).
_PAK_MAGIC = b"EBPK"
_PAK_VERSION = 1
_PAK_HEADER_FORMAT = "<4sII32s"  # magic, version, asset_count, layout_hash
_PAK_HEADER_SIZE = struct.calcsize(_PAK_HEADER_FORMAT)
_PAK_INDEX_ENTRY_FORMAT = "<II"  # offset, length


def _path_to_identifier(path: str) -> str:
    """Convert a relative file path to a valid C identifier."""
    ident = re.sub(r"[^a-zA-Z0-9]", "_", path)
    if ident and ident[0].isdigit():
        ident = "a" + ident
    return "asset_" + ident


def _path_to_enum_name(path: str) -> str:
    """Convert a relative file path to an UPPER_SNAKE_CASE enum name."""
    name = re.sub(r"[^a-zA-Z0-9]", "_", path).upper()
    if name and name[0].isdigit():
        name = "A" + name
    return "ASSET_" + name


def _collect_assets(manifest_path: Path, bin_dir: Path, custom_dir: Path) -> dict[str, Path]:
    """Collect binary asset files listed in the manifest, with custom_dir overrides."""
    assets: dict[str, Path] = {}

    for line in manifest_path.read_text().splitlines():
        rel_path = line.strip()
        if not rel_path:
            continue
        full_path = bin_dir / rel_path
        if full_path.is_file():
            assets[rel_path] = full_path.resolve()

    if custom_dir.is_dir():
        for child in custom_dir.rglob("*"):
            if not child.is_file() or child.name == ".gitkeep":
                continue
            rel_path = child.relative_to(custom_dir).as_posix()
            if "/png/" in rel_path or rel_path.startswith("png/"):
                continue
            assets[rel_path] = child.resolve()

    return assets


def _detect_families(
    paths: list[str],
    locale: str,
) -> tuple[list[str], dict[str, list[tuple[int, str]]]]:
    """Detect parameterized families (assets with purely numeric basenames).

    Locale-prefixed families (e.g. US/maps/palettes/) are merged with their
    common counterparts (maps/palettes/) into a single family keyed by the
    unprefixed directory.  For each index, the locale-specific version is
    preferred if it exists.

    Returns (singletons, families) where:
      singletons: paths that are NOT part of a numeric family
      families: dict mapping family_key -> [(numeric_index, full_path), ...]
    """
    locale_prefix = f"{locale}/"

    groups: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for path in paths:
        parts = path.rsplit("/", 1)
        if len(parts) == 2:
            directory, filename = parts
        else:
            directory, filename = "", parts[0]

        dot_idx = filename.find(".")
        if dot_idx >= 0:
            basename = filename[:dot_idx]
            ext = filename[dot_idx:]
        else:
            basename = filename
            ext = ""

        key = f"{directory}/{ext}" if directory else ext
        groups[key].append((basename, path))

    # First pass: identify numeric families within each group, then merge
    # locale-prefixed families with their common counterparts.
    raw_families: dict[str, list[tuple[int, str]]] = {}
    singletons: list[str] = []

    for key, members in groups.items():
        numeric_members: list[tuple[int, str]] = []
        non_numeric: list[str] = []
        for basename, path in members:
            try:
                idx = int(basename)
                numeric_members.append((idx, path))
            except ValueError:
                non_numeric.append(path)

        # Non-numeric members are always singletons
        singletons.extend(non_numeric)

        if len(numeric_members) >= 2:
            raw_families[key] = numeric_members
        elif numeric_members:
            # Single numeric member — treat as singleton
            singletons.append(numeric_members[0][1])

    # Merge locale-prefixed families with common counterparts.
    # E.g. "US/maps/palettes/.pal" merges into "maps/palettes/.pal".
    families: dict[str, list[tuple[int, str]]] = {}
    for key, members in raw_families.items():
        base_key = key
        for prefix in _LOCALE_PREFIXES:
            if key.startswith(prefix):
                base_key = key[len(prefix) :]
                break
        if base_key not in families:
            families[base_key] = []
        families[base_key].extend(members)

    # Deduplicate: for each index, prefer locale-specific version
    for key in families:
        idx_to_path: dict[int, str] = {}
        for idx, path in families[key]:
            if idx not in idx_to_path or path.startswith(locale_prefix):
                idx_to_path[idx] = path
        families[key] = sorted(idx_to_path.items())

    singletons.sort()
    return singletons, families


def _build_locale_map(all_paths: set[str], locale: str) -> dict[str, str]:
    """Build mapping from unprefixed base path to actual locale-specific path."""
    prefix = f"{locale}/"
    locale_map: dict[str, str] = {}
    for path in all_paths:
        if path.startswith(prefix):
            base = path[len(prefix) :]
            locale_map[base] = path
    return locale_map


def _family_macro_name(dir_part: str) -> str:
    """Generate a family macro name from the directory part."""
    macro_base = re.sub(r"[^a-zA-Z0-9]", "_", dir_part).upper()
    if macro_base and macro_base[0].isdigit():
        macro_base = "A" + macro_base
    return f"ASSET_{macro_base}" if macro_base else "ASSET_FAMILY"


def _family_first_enum_name(entries: list[tuple[int, str | None, str | None]], locale: str) -> str:
    """The enum name that anchors a family's ``MACRO(n)`` addressing.

    ``min_idx`` (== ``entries[0]``'s index) always has a real file — it's
    defined as the smallest index with one — so ``entries[0]`` is never a
    gap. Both codegen paths (the ``MACRO(n)`` macro and the runtime
    per-family array population loop) must anchor on this same enum name,
    since it's what makes ``embedded_assets[anchor + n - min_idx]`` line up
    with ``asset_family_X[n - min_idx]``.
    """
    locale_prefix = f"{locale}/"
    path = entries[0][1]
    assert path is not None, "family's min_idx entry must have a real file"
    base_path = path[len(locale_prefix) :] if path.startswith(locale_prefix) else path
    return _path_to_enum_name(base_path)


# family_key -> (macro_name, min_idx, max_idx, [(idx, path_or_None, incbin_id_or_None), ...])
FamilyInfo = dict[str, tuple[str, int, int, list[tuple[int, str | None, str | None]]]]


@dataclass
class ResolvedAssets:
    """The single deterministic answer to "what is asset N, in what order".

    Computed once by :func:`_resolve_asset_order` and consumed by both the
    compile-time C codegen path (:func:`embed_registry`) and the runtime
    ``.pak`` writer (:func:`pack`), so the two can never assign IDs
    differently for the same ``(manifest, exclude, locale, custom_dir)``.
    """

    assets: dict[str, Path]  # relative path -> resolved absolute file
    sorted_paths: list[str]
    enum_entries: list[tuple[str, str]]  # (enum_name, comment), in AssetId enum order
    enum_to_path: dict[str, str | None]  # enum_name -> relative path (None for a family gap)
    enum_to_incbin: dict[str, str | None]  # enum_name -> INCBIN identifier (None for a gap)
    family_info: FamilyInfo
    locale_aliases: dict[str, str] = field(default_factory=dict)
    locale_data_aliases: dict[str, str] = field(default_factory=dict)
    locale_size_aliases: dict[str, str] = field(default_factory=dict)


def _resolve_asset_order(
    manifest: Path,
    bin_dir: Path,
    custom_dir: Path,
    exclude: list[str] | None,
    locale: str,
) -> ResolvedAssets:
    """Resolve the manifest into a deterministic, fully-ordered asset list.

    This is exactly the ID-assignment logic ``embed_registry`` has always
    used (collect -> exclude -> sort -> detect families -> assign enum
    names in order), factored out so ``pack()`` can reuse it verbatim.
    """
    assets = _collect_assets(manifest, bin_dir, custom_dir)

    if exclude:
        assets = {k: v for k, v in assets.items() if not any(fnmatch.fnmatch(k, pat) for pat in exclude)}

    sorted_paths = sorted(assets.keys())
    if not sorted_paths:
        print("Warning: no assets found", file=sys.stderr)

    locale_map = _build_locale_map(set(sorted_paths), locale)
    singletons, families = _detect_families(sorted_paths, locale)
    path_to_incbin: dict[str, str] = {p: _path_to_identifier(p) for p in sorted_paths}

    enum_entries: list[tuple[str, str]] = []
    enum_to_incbin: dict[str, str | None] = {}
    enum_to_path: dict[str, str | None] = {}
    path_to_enum: dict[str, str] = {}

    for path in singletons:
        enum_name = _path_to_enum_name(path)
        enum_entries.append((enum_name, path))
        enum_to_incbin[enum_name] = path_to_incbin[path]
        enum_to_path[enum_name] = path
        path_to_enum[path] = enum_name

    family_info: FamilyInfo = {}
    locale_prefix = f"{locale}/"

    for key in sorted(families.keys()):
        members = families[key]
        min_idx = members[0][0]
        max_idx = members[-1][0]
        idx_to_path = dict(members)

        parts = key.rsplit("/", 1)
        dir_part = parts[0] if len(parts) == 2 else ""
        ext_part = parts[1] if len(parts) == 2 else parts[0]
        macro_name = _family_macro_name(dir_part)

        entries: list[tuple[int, str | None, str | None]] = []
        for idx in range(min_idx, max_idx + 1):
            path = idx_to_path.get(idx)
            if path:
                # For merged families, always use the unprefixed enum name
                # so family members are contiguous regardless of locale.
                base_path = path[len(locale_prefix) :] if path.startswith(locale_prefix) else path
                enum_name = _path_to_enum_name(base_path)
                incbin_id = path_to_incbin[path]
                enum_entries.append((enum_name, path))
                enum_to_incbin[enum_name] = incbin_id
                enum_to_path[enum_name] = path
                path_to_enum[path] = enum_name
                entries.append((idx, path, incbin_id))
            else:
                sentinel_path = f"{dir_part}/{idx}{ext_part}" if dir_part else f"{idx}{ext_part}"
                enum_name = _path_to_enum_name(sentinel_path)
                enum_entries.append((enum_name, f"[gap] {sentinel_path}"))
                enum_to_incbin[enum_name] = None
                enum_to_path[enum_name] = None
                entries.append((idx, None, None))

        family_info[key] = (macro_name, min_idx, max_idx, entries)

    # Build locale aliases (ASSET_FOO -> ASSET_US_FOO, plus _DATA/_SIZE variants).
    locale_aliases: dict[str, str] = {}
    locale_data_aliases: dict[str, str] = {}
    locale_size_aliases: dict[str, str] = {}
    for base_path, actual_path in locale_map.items():
        if actual_path in path_to_enum:
            target_enum = path_to_enum[actual_path]
            alias_enum = _path_to_enum_name(base_path)
            if alias_enum != target_enum and alias_enum not in enum_to_incbin:
                locale_aliases[alias_enum] = target_enum
                locale_data_aliases[f"{alias_enum}_DATA"] = f"{target_enum}_DATA"
                locale_size_aliases[f"{alias_enum}_SIZE"] = f"{target_enum}_SIZE"

    return ResolvedAssets(
        assets=assets,
        sorted_paths=sorted_paths,
        enum_entries=enum_entries,
        enum_to_path=enum_to_path,
        enum_to_incbin=enum_to_incbin,
        family_info=family_info,
        locale_aliases=locale_aliases,
        locale_data_aliases=locale_data_aliases,
        locale_size_aliases=locale_size_aliases,
    )


def _dump_entries_to_offset_table(doc: "DumpDoc") -> dict[str, tuple[int, int]]:
    """Map each raw-binary asset's manifest relative path to its (rom_offset, rom_size).

    Mirrors the filename ebtools.parsers.raw.write_raw()/ebtools.cli.extract's
    dump_data() produce for every ``dumpEntries`` entry whose extension isn't
    one of the PARSERS (those produce assembly source instead of a raw binary
    file, and are already excluded from assets.manifest -- see extract.py).
    Lets the runtime ROM extractor (src/data/rom_extract.c) reconstruct a
    binary asset directly from a donor ROM's byte range, without needing an
    actual asm/bin/ extraction on disk.
    """
    from ebtools.parsers import PARSERS

    table: dict[str, tuple[int, int]] = {}
    for entry in doc.dumpEntries:
        if entry.extension in PARSERS:
            continue
        if entry.compressed:
            filename = f"{entry.name}.{entry.extension}.lzhal"
        else:
            filename = f"{entry.name}.{entry.extension}"
        rel_path = f"{entry.subdir}/{filename}" if entry.subdir else filename
        table[rel_path] = (entry.offset, entry.size)
    return table


def _compute_layout_hash(enum_entries: list[tuple[str, str]]) -> bytes:
    """32-byte SHA-256 identity hash of an asset *layout*, not its bytes.

    Hashes the ordered list of enum names (i.e. what ``ASSET_COUNT`` and
    each ``AssetId`` slot mean), not the manifest file or any asset
    content. Two builds that produce the same hash are guaranteed to agree
    on the meaning of every asset ID, regardless of which ROM's bytes filled
    them in. A runtime loader compares this against the value baked into
    the binary to refuse a stale/mismatched ``assets.pak``.
    """
    names = "\n".join(enum_name for enum_name, _comment in enum_entries)
    return hashlib.sha256(names.encode("utf-8")).digest()


def embed_registry(
    manifest: Path,
    bin_dir: Path,
    output_dir: Path,
    incbin_dir: Path,
    *,
    custom_dir: Path = Path("src/custom_assets"),
    exclude: list[str] | None = None,
    locale: str = "US",
    runtime: Annotated[
        bool,
        Parameter(help="Generate metadata-only headers for the EB_RUNTIME_ASSETS build (no asset bytes, no INCBIN)."),
    ] = False,
    rom_yaml: Annotated[
        Path,
        Parameter(help="Dump doc YAML (only read in --runtime mode, to emit rom_extract_table.c/.h)."),
    ] = Path("earthbound.yml"),
) -> None:
    """Generate C source files for embedding binary assets using incbin.h.

    Parameters
    ----------
    manifest
        Path to assets.manifest (generated during extraction).
    bin_dir
        Path to extracted ROM assets (e.g. asm/bin/).
    output_dir
        Directory for generated C files.
    incbin_dir
        Path to directory containing incbin.h.
    custom_dir
        Path to custom asset overrides.
    exclude
        Glob patterns for asset paths to exclude (e.g. ``audiopacks/*``).
    locale
        Build locale for resolving locale-specific assets (US or JP).
    runtime
        Generate the metadata-only variant used by EB_RUNTIME_ASSETS builds:
        skips embedded_assets.inc.c / embedded_assets_decl.h /
        embedded_assets_array.c (no ROM bytes involved), declares
        embedded_assets/asset_family_* as non-const (populated by the
        runtime loader instead of link-time INCBIN), and additionally
        writes asset_pack_layout.c/.h (the family population code + the
        layout hash an assets.pak is checked against) plus
        rom_extract_table.c/.h (per-asset ROM byte ranges, so the compiled
        game can build its own assets.pak straight from a donor ROM with no
        Python involved -- see src/data/rom_extract.c).
    rom_yaml
        Dump doc YAML, only read in --runtime mode.
    """
    output_dir.mkdir(parents=True, exist_ok=True)

    resolved = _resolve_asset_order(manifest, bin_dir, custom_dir, exclude, locale)
    sorted_paths = resolved.sorted_paths
    enum_entries = resolved.enum_entries
    enum_to_incbin = resolved.enum_to_incbin
    family_info = resolved.family_info

    header = "/* Auto-generated by ebtools embed-registry - do not edit */\n"
    incbin_path = incbin_dir.resolve().as_posix()
    const_kw = "" if runtime else "const "

    # --- asset_pack_hash.h: the layout hash, in both modes. Only depends on
    # (manifest, exclude, locale) -- not on runtime vs compile-time-embed --
    # so a compile-time-embed build (which has every asset's bytes on hand
    # already) can export a valid assets.pak for a runtime-mode build to
    # read later, as long as both were generated from the same inputs. See
    # src/data/pak_export.c. ---
    layout_hash_hex = _compute_layout_hash(enum_entries).hex()
    hash_hdr_lines = [
        header,
        "#ifndef ASSET_PACK_HASH_H",
        "#define ASSET_PACK_HASH_H",
        "",
        f'#define ASSET_PACK_LAYOUT_HASH "{layout_hash_hex}"',
        "",
        "#endif /* ASSET_PACK_HASH_H */",
        "",
    ]
    (output_dir / "asset_pack_hash.h").write_text("\n".join(hash_hdr_lines))

    if not runtime:
        # --- embedded_assets.inc.c (INCBIN definitions) ---
        inc_path = output_dir / "embedded_assets.inc.c"
        incbin_lines = [f'INCBIN({_path_to_identifier(p)}, "{resolved.assets[p].as_posix()}");' for p in sorted_paths]
        inc_path.write_text(header + "\n" + "\n".join(incbin_lines) + "\n")

        # --- embedded_assets_decl.h (extern declarations for INCBIN symbols) ---
        decl_path = output_dir / "embedded_assets_decl.h"
        decl_lines = [header, "#ifndef EMBEDDED_ASSETS_DECL_H", "#define EMBEDDED_ASSETS_DECL_H", ""]
        for p in sorted_paths:
            ident = _path_to_identifier(p)
            decl_lines.append(f"extern const unsigned char {ident}Data[];")
            decl_lines.append(f"extern const unsigned int {ident}Size;")
        decl_lines.extend(["", "#endif /* EMBEDDED_ASSETS_DECL_H */", ""])
        decl_path.write_text("\n".join(decl_lines))

    # ===================================================================
    # Generate asset_ids.h
    # ===================================================================
    ids_lines = [
        header,
        "#ifndef ASSET_IDS_H",
        "#define ASSET_IDS_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
    ]
    if not runtime:
        ids_lines.append('#include "embedded_assets_decl.h"')
    ids_lines.append("")

    # Enum (kept for family indexing and backward compat)
    ids_lines.append("typedef enum {")
    for enum_name, comment in enum_entries:
        ids_lines.append(f"    {enum_name}, /* {comment} */")
    ids_lines.append(f"    ASSET_COUNT  /* {len(enum_entries)} */")
    ids_lines.append("} AssetId;")
    ids_lines.append("")

    # Direct-access macros for each asset: ASSET_FOO_DATA / ASSET_FOO_SIZE.
    # Always routed through embedded_assets[] (not a direct INCBIN symbol
    # reference) so the same header works whether that array was populated
    # at link time (compile-time embed) or at startup (runtime loader).
    ids_lines.append("/* Direct-access macros: same asset, addressed by name instead of index */")
    for enum_name, _comment in enum_entries:
        ids_lines.append(f"#define {enum_name}_DATA ((const uint8_t *)embedded_assets[{enum_name}].data)")
        ids_lines.append(f"#define {enum_name}_SIZE ((size_t)*embedded_assets[{enum_name}].size_ptr)")
    ids_lines.append("")

    # Locale-resolved aliases (enum + _DATA + _SIZE)
    if resolved.locale_aliases:
        ids_lines.append("/* Locale-resolved aliases */")
        for alias, target in sorted(resolved.locale_aliases.items()):
            ids_lines.append(f"#define {alias} {target}")
        for alias, target in sorted(resolved.locale_data_aliases.items()):
            ids_lines.append(f"#define {alias} {target}")
        for alias, target in sorted(resolved.locale_size_aliases.items()):
            ids_lines.append(f"#define {alias} {target}")
        ids_lines.append("")

    # Per-family array declarations and accessor macros
    ids_lines.append("/* Per-family lookup (for runtime-indexed access) */")
    ids_lines.append("typedef struct { const unsigned char *data; const unsigned int *size_ptr; } AssetFamilyEntry;")
    ids_lines.append("")

    for key in sorted(family_info.keys()):
        macro_name, min_idx, max_idx, entries = family_info[key]
        count = max_idx - min_idx + 1
        array_name = f"asset_family_{macro_name.lower()[6:]}"  # strip ASSET_ prefix

        first_enum_name = _family_first_enum_name(entries, locale)

        # Enum-style macro: ASSET_FAMILY(n) → enum value (for use with ASSET_DATA/ASSET_SIZE)
        if min_idx == 0:
            ids_lines.append(f"#define {macro_name}(n) ({first_enum_name} + (n))")
        else:
            ids_lines.append(f"#define {macro_name}(n) ({first_enum_name} + (n) - {min_idx})")

        # Per-family array + direct-access macros
        ids_lines.append(f"extern {const_kw}AssetFamilyEntry {array_name}[{count}];")
        if min_idx == 0:
            ids_lines.append(f"#define {macro_name}_DATA(n) ((const uint8_t *){array_name}[n].data)")
            ids_lines.append(f"#define {macro_name}_SIZE(n) ((size_t)*{array_name}[n].size_ptr)")
        else:
            ids_lines.append(f"#define {macro_name}_DATA(n) ((const uint8_t *){array_name}[(n) - {min_idx}].data)")
            ids_lines.append(f"#define {macro_name}_SIZE(n) ((size_t)*{array_name}[(n) - {min_idx}].size_ptr)")

    ids_lines.extend(["", "#endif /* ASSET_IDS_H */", ""])

    ids_path = output_dir / "asset_ids.h"
    ids_path.write_text("\n".join(ids_lines))

    # ===================================================================
    # Generate embedded_assets.h (declares AssetEntry + global array)
    # ===================================================================
    hdr_path = output_dir / "embedded_assets.h"
    hdr_path.write_text(
        header
        + textwrap.dedent(f"""\
        #ifndef EMBEDDED_ASSETS_H
        #define EMBEDDED_ASSETS_H

        #include "asset_ids.h"

        /* Asset entry: data pointer + pointer to a size variable. In the
         * compile-time-embed build both fields are link-time constants
         * (INCBIN); in an EB_RUNTIME_ASSETS build they're populated by
         * eb_runtime_assets_load() before first use. */
        typedef struct {{
            const unsigned char *data;
            const unsigned int *size_ptr;
        }} AssetEntry;

        extern {const_kw}AssetEntry embedded_assets[ASSET_COUNT];

        #endif /* EMBEDDED_ASSETS_H */
    """)
    )

    if not runtime:
        # ===================================================================
        # Generate embedded_assets_array.c (INCBIN definitions + family arrays)
        # ===================================================================
        inc_path = output_dir / "embedded_assets.inc.c"
        array_lines = [
            header,
            "#define INCBIN_PREFIX",
            f'#include "{incbin_path}/incbin.h"',
            '#include "asset_ids.h"',
            '#include "embedded_assets.h"',
            "",
            f'#include "{inc_path.resolve().as_posix()}"',
            "",
            "/* Section attribute for the asset index tables. Default: empty.",
            " * Embedded ports (e.g. STM32 Game & Watch) override via -D to place",
            " * the tables in a specific linker section so a runtime relocation",
            " * pass can fix up their data/size_ptr fields. */",
            "#ifndef EBASSET_TABLE_ATTR",
            "#define EBASSET_TABLE_ATTR",
            "#endif",
            "",
        ]

        # Global array: const-initialized with {data_ptr, &size_ptr} pairs.
        # Both are link-time constants, so no runtime init needed.
        array_lines.append("EBASSET_TABLE_ATTR const AssetEntry embedded_assets[ASSET_COUNT] = {")
        for enum_name, _comment in enum_entries:
            incbin_id = enum_to_incbin.get(enum_name)
            if incbin_id is not None:
                array_lines.append(f"    [{enum_name}] = {{ {incbin_id}Data, &{incbin_id}Size }},")
            else:
                array_lines.append(f"    [{enum_name}] = {{ 0, 0 }},")
        array_lines.append("};")
        array_lines.append("")

        # Per-family arrays
        for key in sorted(family_info.keys()):
            macro_name, min_idx, max_idx, entries = family_info[key]
            count = max_idx - min_idx + 1
            array_name = f"asset_family_{macro_name.lower()[6:]}"

            array_lines.append(f"EBASSET_TABLE_ATTR const AssetFamilyEntry {array_name}[{count}] = {{")
            for idx, _path, incbin_id in entries:
                if incbin_id is not None:
                    array_lines.append(f"    {{ {incbin_id}Data, &{incbin_id}Size }}, /* {idx} */")
                else:
                    array_lines.append(f"    {{ 0, 0 }}, /* {idx} [gap] */")
            array_lines.append("};")
            array_lines.append("")

        array_path = output_dir / "embedded_assets_array.c"
        array_path.write_text("\n".join(array_lines))
    else:
        # ===================================================================
        # Generate asset_pack_layout.h/.c. embedded_assets.h and asset_ids.h
        # only *declare* embedded_assets[]/asset_family_*[] (`extern`, no
        # storage) in runtime mode — nothing else generates a compile-time
        # embed build, so asset_pack_layout.c is where their actual storage
        # lives (zero-initialized, populated later by the runtime loader).
        # It also carries the layout hash an assets.pak is checked against,
        # and the code that copies each family's slice out of
        # embedded_assets[] once the loader has filled it in. Family
        # members are contiguous AssetId ranges by construction, so that's
        # just `dst[i] = embedded_assets[first+i]` per family — generated
        # here instead of interpreted generically at runtime, matching how
        # embedded_assets_array.c's initializer is generated for the
        # compile-time-embed build.
        # ===================================================================
        layout_hash_hex = _compute_layout_hash(enum_entries).hex()

        layout_hdr_lines = [
            header,
            "#ifndef ASSET_PACK_LAYOUT_H",
            "#define ASSET_PACK_LAYOUT_H",
            "",
            f'#define ASSET_PACK_LAYOUT_HASH "{layout_hash_hex}"',
            "",
            "/* Populates every asset_family_* array from embedded_assets[], which",
            " * must already be populated (see eb_runtime_assets_load()). */",
            "void eb_runtime_assets_populate_families(void);",
            "",
            "#endif /* ASSET_PACK_LAYOUT_H */",
            "",
        ]
        (output_dir / "asset_pack_layout.h").write_text("\n".join(layout_hdr_lines))

        layout_src_lines = [
            header,
            '#include "asset_ids.h"',
            '#include "embedded_assets.h"',
            '#include "asset_pack_layout.h"',
            "",
            "/* Storage for the runtime-populated tables declared `extern` in",
            " * embedded_assets.h / asset_ids.h. Zero-initialized; the runtime",
            " * loader fills these in (eb_runtime_assets_load() then this file's",
            " * eb_runtime_assets_populate_families()). */",
            "AssetEntry embedded_assets[ASSET_COUNT];",
        ]
        for key in sorted(family_info.keys()):
            macro_name, min_idx, max_idx, entries = family_info[key]
            count = max_idx - min_idx + 1
            array_name = f"asset_family_{macro_name.lower()[6:]}"
            layout_src_lines.append(f"AssetFamilyEntry {array_name}[{count}];")
        layout_src_lines.append("")
        layout_src_lines.append("void eb_runtime_assets_populate_families(void) {")
        for key in sorted(family_info.keys()):
            macro_name, min_idx, max_idx, entries = family_info[key]
            count = max_idx - min_idx + 1
            array_name = f"asset_family_{macro_name.lower()[6:]}"
            first_enum_name = _family_first_enum_name(entries, locale)
            # AssetEntry and AssetFamilyEntry are distinct (if identically
            # shaped) struct types, so this copies field-by-field rather
            # than assigning the struct directly.
            layout_src_lines.append(f"    for (unsigned i = 0; i < {count}u; i++) {{ /* {key} */")
            layout_src_lines.append(
                f"        {array_name}[i].data = embedded_assets[{first_enum_name} + i].data;"
            )
            layout_src_lines.append(
                f"        {array_name}[i].size_ptr = embedded_assets[{first_enum_name} + i].size_ptr;"
            )
            layout_src_lines.append("    }")
        layout_src_lines.append("}")
        layout_src_lines.append("")
        (output_dir / "asset_pack_layout.c").write_text("\n".join(layout_src_lines))

        # ===================================================================
        # Generate rom_extract_table.h/.c: per-asset ROM byte ranges, so
        # src/data/rom_extract.c can build an assets.pak straight from a
        # donor ROM without any Python/ebtools involvement at runtime. Only
        # covers raw-binary entries (same set assets.manifest already has --
        # PARSERS-handled entries produce assembly source, not a binary
        # asset, and were never part of the runtime asset layout).
        # ===================================================================
        from ebtools.config import load_dump_doc
        from ebtools.rom import ROM_SIZE

        if rom_yaml.is_file():
            doc = load_dump_doc(rom_yaml)
            offset_table = _dump_entries_to_offset_table(doc)
            rom_identifier = doc.romIdentifier
        else:
            # No dump doc available (e.g. a small test fixture with no real
            # earthbound.yml) -- still emit a valid, ASSET_COUNT-sized table
            # so anything depending on rom_extract_table.c/.h still builds;
            # every entry is just a gap (rom_size 0), so rom_extract_build_pak()
            # would produce an all-empty pak, not a wrong one.
            print(
                f"Note: {rom_yaml} not found, writing an empty rom_extract_table.c "
                "(fine for a test fixture; a real build needs the real YAML).",
                file=sys.stderr,
            )
            offset_table = {}
            rom_identifier = ""

        table_hdr_lines = [
            header,
            "#ifndef ROM_EXTRACT_TABLE_H",
            "#define ROM_EXTRACT_TABLE_H",
            "",
            "#include <stdint.h>",
            '#include "asset_ids.h"',
            "",
            f'#define ROM_IDENTIFIER "{rom_identifier}"',
            f"#define ROM_SIZE {ROM_SIZE}u",
            "",
            "typedef struct {",
            "    uint32_t rom_offset;",
            "    uint32_t rom_size;  /* 0 means this asset has no ROM-derived source (a family gap) */",
            "} RomExtractEntry;",
            "",
            "extern const RomExtractEntry rom_extract_table[ASSET_COUNT];",
            "",
            "/* Source .arr.lzhal ranges for the 34 PSI arrangements (see",
            " * ebtools/parsers/psi_arrangements.py). These never appear in",
            " * rom_extract_table[] -- the final asset is the *bundled* form,",
            " * built from these at runtime by rom_extract.c (decompress, split",
            " * into 8-frame chunks, re-encode each chunk). */",
            "#define PSI_ARRANGEMENT_COUNT 34",
            "extern const RomExtractEntry psi_arrangement_source[PSI_ARRANGEMENT_COUNT];",
            "",
            "#endif /* ROM_EXTRACT_TABLE_H */",
            "",
        ]
        (output_dir / "rom_extract_table.h").write_text("\n".join(table_hdr_lines))

        table_src_lines = [
            header,
            '#include "rom_extract_table.h"',
            "",
            "const RomExtractEntry rom_extract_table[ASSET_COUNT] = {",
        ]
        missing = 0
        overridden = 0
        for enum_name, _comment in enum_entries:
            path = resolved.enum_to_path.get(enum_name)
            entry = offset_table.get(path) if path else None
            # A path can have BOTH a raw ROM byte range (earthbound.yml
            # dumpEntries) AND a custom_dir override (src/custom_assets/,
            # via pack-all) -- when both exist, _collect_assets() makes the
            # override win for the real embedded build (see its "if
            # custom_dir.is_dir(): ... assets[rel_path] = child" loop
            # above). A raw ROM extraction would silently produce the
            # *wrong* (original, non-customized) bytes in that case, e.g.
            # this project's own custom Don't Care name list overriding
            # the ROM's default names -- so treat an overridden path as a
            # gap too, same as one with no ROM source at all.
            if entry is not None and path is not None:
                actual_src = resolved.assets.get(path)
                raw_src = (bin_dir / path)
                if actual_src is not None and raw_src.is_file() and actual_src.resolve() != raw_src.resolve():
                    entry = None
                    overridden += 1
            if entry is None:
                missing += 1
                table_src_lines.append(f"    [{enum_name}] = {{ 0, 0 }},")
            else:
                off, size = entry
                table_src_lines.append(f"    [{enum_name}] = {{ {off}u, {size}u }}, /* {path} */")
        table_src_lines.append("};")
        table_src_lines.append("")

        table_src_lines.append("const RomExtractEntry psi_arrangement_source[PSI_ARRANGEMENT_COUNT] = {")
        psi_missing = 0
        for i in range(34):
            src_path = f"psianims/arrangements/{i}.arr.lzhal"
            entry = offset_table.get(src_path)
            if entry is None:
                psi_missing += 1
                table_src_lines.append(f"    [{i}] = {{ 0, 0 }},")
            else:
                off, size = entry
                table_src_lines.append(f"    [{i}] = {{ {off}u, {size}u }}, /* {src_path} */")
        table_src_lines.append("};")
        table_src_lines.append("")
        (output_dir / "rom_extract_table.c").write_text("\n".join(table_src_lines))
        if psi_missing:
            print(f"Note: {psi_missing} PSI arrangement source(s) missing from earthbound.yml.", file=sys.stderr)
        if missing:
            print(
                f"Note: {missing} asset(s) have no ROM-derived byte range in rom_extract_table.c "
                f"(family gaps, custom_dir-only assets, or custom_dir overrides of an "
                f"otherwise-ROM-derivable path -- {overridden} of these are overrides) -- these "
                "will be zero-length in a ROM-built assets.pak.",
                file=sys.stderr,
            )

    print(
        f"Generated {len(sorted_paths)} embedded assets ({len(enum_entries)} enum entries, "
        f"{len(family_info)} families, {len(resolved.locale_aliases)} locale aliases) "
        f"in {output_dir}{' [runtime metadata only]' if runtime else ''}"
    )


def pack(
    manifest: Path,
    bin_dir: Path,
    output_pak: Path,
    *,
    custom_dir: Path = Path("src/custom_assets"),
    exclude: list[str] | None = None,
    locale: str = "US",
) -> None:
    """Pack extracted assets into a single ``assets.pak`` for runtime loading.

    Uses the exact same asset ordering as ``embed_registry(..., runtime=True)``
    (both call :func:`_resolve_asset_order`), so a pack built with the same
    ``(manifest, exclude, locale, custom_dir)`` a maintainer used to generate
    the committed runtime metadata will always match the layout hash baked
    into an EB_RUNTIME_ASSETS binary.

    Parameters
    ----------
    manifest
        Path to assets.manifest (generated during extraction).
    bin_dir
        Path to extracted ROM assets (e.g. asm/bin/).
    output_pak
        Path to write the packed ``assets.pak`` file to.
    custom_dir
        Path to custom asset overrides.
    exclude
        Glob patterns for asset paths to exclude (e.g. ``audiopacks/*``).
    locale
        Build locale for resolving locale-specific assets (US or JP).
    """
    resolved = _resolve_asset_order(manifest, bin_dir, custom_dir, exclude, locale)
    layout_hash = _compute_layout_hash(resolved.enum_entries)

    index_entries: list[tuple[int, int]] = []
    blob = bytearray()
    for enum_name, _comment in resolved.enum_entries:
        rel_path = resolved.enum_to_path.get(enum_name)
        if rel_path is None:
            index_entries.append((0, 0))
            continue
        data = resolved.assets[rel_path].read_bytes()
        offset = len(blob)
        blob.extend(data)
        index_entries.append((offset, len(data)))

    asset_count = len(resolved.enum_entries)
    header_bytes = struct.pack(_PAK_HEADER_FORMAT, _PAK_MAGIC, _PAK_VERSION, asset_count, layout_hash)
    index_bytes = b"".join(struct.pack(_PAK_INDEX_ENTRY_FORMAT, offset, length) for offset, length in index_entries)

    output_pak.parent.mkdir(parents=True, exist_ok=True)
    with output_pak.open("wb") as f:
        f.write(header_bytes)
        f.write(index_bytes)
        f.write(blob)

    print(
        f"Packed {asset_count} assets ({len(blob)} bytes blob, "
        f"layout hash {layout_hash.hex()[:16]}...) to {output_pak}"
    )

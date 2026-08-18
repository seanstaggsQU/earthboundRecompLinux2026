"""Tests for the shared asset-ordering helper and the assets.pak writer.

Covers the invariant the runtime loader depends on: `embed_registry(...,
runtime=True)` and `pack(...)` called with the same
(manifest, exclude, locale, custom_dir) must agree on ASSET_COUNT and on
the layout hash, because they both resolve IDs through the same
`_resolve_asset_order` helper.
"""

import re
import struct

import pytest

from ebtools.cli.embed import (
    _PAK_HEADER_FORMAT,
    _PAK_HEADER_SIZE,
    _PAK_INDEX_ENTRY_FORMAT,
    _compute_layout_hash,
    _resolve_asset_order,
    embed_registry,
    pack,
)

# (relative_path, content) — a couple of singletons, a locale-only singleton
# (no unprefixed counterpart, so it gets a locale alias), and a 4-slot
# numeric family with a gap at index 2.
_FILES = {
    "data/foo.bin": b"foo-bytes",
    "data/bar.bin": b"bar-bytes-longer",
    "US/data/greeting.bin": b"hello",
    "maps/palettes/0.pal": b"\x00" * 32,
    "maps/palettes/1.pal": b"\x01" * 32,
    "maps/palettes/3.pal": b"\x03" * 32,
    # index 2 deliberately missing -> gap entry
}


@pytest.fixture
def fixture_dirs(tmp_path):
    bin_dir = tmp_path / "bin"
    for rel_path, content in _FILES.items():
        full = bin_dir / rel_path
        full.parent.mkdir(parents=True, exist_ok=True)
        full.write_bytes(content)

    manifest = tmp_path / "assets.manifest"
    manifest.write_text("\n".join(_FILES) + "\n")

    custom_dir = tmp_path / "custom_assets"  # deliberately absent
    return manifest, bin_dir, custom_dir


def test_resolve_asset_order_ids_and_families(fixture_dirs):
    manifest, bin_dir, custom_dir = fixture_dirs
    resolved = _resolve_asset_order(manifest, bin_dir, custom_dir, exclude=None, locale="US")

    names = [name for name, _comment in resolved.enum_entries]
    assert names == [
        "ASSET_US_DATA_GREETING_BIN",
        "ASSET_DATA_BAR_BIN",
        "ASSET_DATA_FOO_BIN",
        "ASSET_MAPS_PALETTES_0_PAL",
        "ASSET_MAPS_PALETTES_1_PAL",
        "ASSET_MAPS_PALETTES_2_PAL",  # gap
        "ASSET_MAPS_PALETTES_3_PAL",
    ]

    # The gap has no backing file; everything else does.
    assert resolved.enum_to_path["ASSET_MAPS_PALETTES_2_PAL"] is None
    assert resolved.enum_to_path["ASSET_DATA_FOO_BIN"] == "data/foo.bin"
    assert resolved.enum_to_path["ASSET_MAPS_PALETTES_3_PAL"] == "maps/palettes/3.pal"

    # US/data/greeting.bin has no unprefixed counterpart, so it gets a
    # locale alias pointing the plain name at the US-prefixed enum.
    assert resolved.locale_aliases == {"ASSET_DATA_GREETING_BIN": "ASSET_US_DATA_GREETING_BIN"}

    macro_name, min_idx, max_idx, entries = resolved.family_info["maps/palettes/.pal"]
    assert macro_name == "ASSET_MAPS_PALETTES"
    assert (min_idx, max_idx) == (0, 3)
    assert [idx for idx, _path, _incbin in entries] == [0, 1, 2, 3]


def test_exclude_changes_asset_count(fixture_dirs):
    manifest, bin_dir, custom_dir = fixture_dirs
    full = _resolve_asset_order(manifest, bin_dir, custom_dir, exclude=None, locale="US")
    excluded = _resolve_asset_order(manifest, bin_dir, custom_dir, exclude=["maps/palettes/*"], locale="US")

    assert len(excluded.enum_entries) < len(full.enum_entries)
    assert "maps/palettes/.pal" not in excluded.family_info
    # ...and therefore changes the layout hash too, not just the manifest
    # (which is unaffected by --exclude — see plan deviation #2).
    assert _compute_layout_hash(full.enum_entries) != _compute_layout_hash(excluded.enum_entries)


def test_pack_header_and_index_roundtrip(fixture_dirs, tmp_path):
    manifest, bin_dir, custom_dir = fixture_dirs
    resolved = _resolve_asset_order(manifest, bin_dir, custom_dir, exclude=None, locale="US")
    expected_hash = _compute_layout_hash(resolved.enum_entries)

    output_pak = tmp_path / "assets.pak"
    pack(manifest, bin_dir, output_pak, custom_dir=custom_dir, exclude=None, locale="US")

    raw = output_pak.read_bytes()
    magic, version, asset_count, layout_hash = struct.unpack_from(_PAK_HEADER_FORMAT, raw, 0)

    assert magic == b"EBPK"
    assert version == 1
    assert asset_count == len(resolved.enum_entries) == 7
    assert layout_hash == expected_hash

    index_offset = _PAK_HEADER_SIZE
    entry_size = struct.calcsize(_PAK_INDEX_ENTRY_FORMAT)
    blob_start = index_offset + entry_size * asset_count
    blob = raw[blob_start:]

    for i, (enum_name, _comment) in enumerate(resolved.enum_entries):
        offset, length = struct.unpack_from(_PAK_INDEX_ENTRY_FORMAT, raw, index_offset + i * entry_size)
        rel_path = resolved.enum_to_path[enum_name]
        if rel_path is None:
            assert (offset, length) == (0, 0)
            continue
        expected_bytes = _FILES[rel_path]
        assert length == len(expected_bytes)
        assert blob[offset : offset + length] == expected_bytes


def test_pack_and_runtime_codegen_agree(fixture_dirs, tmp_path):
    """The invariant the C loader's validation depends on."""
    manifest, bin_dir, custom_dir = fixture_dirs

    output_pak = tmp_path / "assets.pak"
    pack(manifest, bin_dir, output_pak, custom_dir=custom_dir, exclude=None, locale="US")
    header = output_pak.read_bytes()[:_PAK_HEADER_SIZE]
    _magic, _version, pak_asset_count, pak_layout_hash = struct.unpack(_PAK_HEADER_FORMAT, header)

    gen_dir = tmp_path / "generated"
    embed_registry(
        manifest,
        bin_dir,
        gen_dir,
        incbin_dir=tmp_path / "vendor" / "incbin",  # never read in runtime mode
        custom_dir=custom_dir,
        exclude=None,
        locale="US",
        runtime=True,
    )

    ids_h = (gen_dir / "asset_ids.h").read_text()
    assert "ASSET_COUNT  /* 7 */" in ids_h
    assert pak_asset_count == 7

    layout_h = (gen_dir / "asset_pack_layout.h").read_text()
    match = re.search(r'ASSET_PACK_LAYOUT_HASH "([0-9a-f]+)"', layout_h)
    assert match is not None
    assert match.group(1) == pak_layout_hash.hex()

    # Runtime mode shouldn't touch ROM bytes: no INCBIN files written.
    assert not (gen_dir / "embedded_assets.inc.c").exists()
    assert not (gen_dir / "embedded_assets_array.c").exists()

    # embedded_assets[] and the family array must be non-const (populated
    # by the loader), unlike the compile-time-embed path.
    embedded_h = (gen_dir / "embedded_assets.h").read_text()
    assert "extern AssetEntry embedded_assets[ASSET_COUNT];" in embedded_h
    assert "extern AssetFamilyEntry asset_family_maps_palettes[4];" in ids_h

    # The family population loop anchors on the same enum the MACRO(n)
    # accessor does, and covers the full [min_idx, max_idx] range.
    layout_c = (gen_dir / "asset_pack_layout.c").read_text()
    assert "for (unsigned i = 0; i < 4u; i++) { /* maps/palettes/.pal */" in layout_c
    assert "asset_family_maps_palettes[i].data = embedded_assets[ASSET_MAPS_PALETTES_0_PAL + i].data;" in layout_c

    # embedded_assets.h/asset_ids.h only *declare* these (extern); the
    # actual storage must be defined exactly once, here.
    assert "AssetEntry embedded_assets[ASSET_COUNT];" in layout_c
    assert "AssetFamilyEntry asset_family_maps_palettes[4];" in layout_c


def test_embed_registry_default_mode_unaffected(fixture_dirs, tmp_path):
    """runtime=False (the default) must keep producing the compile-time-embed output."""
    manifest, bin_dir, custom_dir = fixture_dirs
    gen_dir = tmp_path / "generated"
    incbin_dir = tmp_path / "vendor" / "incbin"
    incbin_dir.mkdir(parents=True)
    (incbin_dir / "incbin.h").write_text("/* stub */\n")

    embed_registry(manifest, bin_dir, gen_dir, incbin_dir, custom_dir=custom_dir, exclude=None, locale="US")

    assert (gen_dir / "embedded_assets.inc.c").exists()
    assert (gen_dir / "embedded_assets_array.c").exists()
    assert not (gen_dir / "asset_pack_layout.c").exists()

    embedded_h = (gen_dir / "embedded_assets.h").read_text()
    assert "extern const AssetEntry embedded_assets[ASSET_COUNT];" in embedded_h

    array_c = (gen_dir / "embedded_assets_array.c").read_text()
    assert "const AssetEntry embedded_assets[ASSET_COUNT] = {" in array_c

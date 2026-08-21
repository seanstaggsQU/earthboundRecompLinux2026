"""The ``setup`` command: build assets.pak from a donor ROM in one step.

Thin wrapper around the existing, already-tested extract -> pack-all ->
pack-assets pipeline (same functions the normal dev workflow and CI use),
just chained together with sane defaults so a player (or the bundled
standalone helper the game shells out to -- see src/data/rom_extract.c)
can get a working assets.pak with a single command.
"""

import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Annotated

from cyclopts import Parameter

from ebtools.default_asset_excludes import DEFAULT_ASSET_EXCLUDES


def _bundled_data_dir() -> Path | None:
    """Directory holding a PyInstaller-frozen bundle's bundled data files,
    if this is running as one (see the --onefile spec's `datas` list).
    None when running normally (from source / an installed package)."""
    meipass = getattr(sys, "_MEIPASS", None)
    return Path(meipass) if meipass else None


def _default_config_path(bundled_name: str, dev_relative: str) -> Path:
    """Resolve a config file/directory: the bundled copy first (frozen
    standalone executable, see the --onefile spec's `datas` list), then a
    source-checkout-relative default (running from within the repo)."""
    bundled = _bundled_data_dir()
    if bundled is not None and (bundled / bundled_name).exists():
        return bundled / bundled_name
    return Path(dev_relative)


def setup(
    rom_path: Path,
    *,
    out: Annotated[
        Path | None,
        Parameter(help="Output assets.pak path. Defaults to the platform-conventional data dir (same one the game itself checks)."),
    ] = None,
    yaml_config: Annotated[Path | None, Parameter(help="Dump doc YAML (defaults to the bundled copy, or ./earthbound.yml).")] = None,
    commondata: Annotated[Path | None, Parameter(help="Common data definitions (defaults to the bundled copy, or ./commondefs.yml).")] = None,
    locale: Annotated[str, Parameter(help="Build locale (US or JP).")] = "US",
) -> None:
    """Build assets.pak from your own EarthBound ROM in one step.

    Runs the same extract -> pack-all -> pack-assets pipeline the normal
    build uses, into a scratch directory, then writes the final pak. Your
    ROM is only ever read from, never modified or copied anywhere
    permanent.

    Parameters
    ----------
    rom_path
        Path to your EarthBound (USA) ROM (.sfc/.smc).
    out
        Where to write assets.pak.
    yaml_config
        Dump doc YAML.
    commondata
        Common data definitions.
    locale
        Build locale (US or JP).
    """
    from ebtools.cli.embed import pack as pack_assets
    from ebtools.cli.extract import extract
    from ebtools.cli.pack_all import pack_all
    from ebtools.data.runtime_assets_paths import default_assets_pak_path

    yaml_config = yaml_config or _default_config_path("earthbound.yml", "earthbound.yml")
    commondata = commondata or _default_config_path("commondefs.yml", "commondefs.yml")
    out = out or default_assets_pak_path()

    if not yaml_config.is_file():
        print(f"Error: dump doc YAML not found at {yaml_config}", file=sys.stderr)
        sys.exit(1)
    if not commondata.is_file():
        print(f"Error: common data not found at {commondata}", file=sys.stderr)
        sys.exit(1)
    if not rom_path.is_file():
        print(f"Error: ROM not found at {rom_path}", file=sys.stderr)
        sys.exit(1)

    # yaml_config/commondata/rom_path/out are resolved to absolute paths
    # before the chdir below, so they still work after CWD changes.
    yaml_config = yaml_config.resolve()
    commondata = commondata.resolve()
    rom_path = rom_path.resolve()
    out = out.resolve()

    with tempfile.TemporaryDirectory(prefix="ebtools-setup-") as scratch_str:
        scratch = Path(scratch_str)
        bin_dir = scratch / "bin"
        packed_dir = scratch / "packed_assets"

        # extract()'s human-friendly asset export (PNG/JSON, phase 2) is
        # hardcoded to write to "port/assets" *relative to the current
        # working directory*, regardless of any dump_path override --
        # that's a pre-existing quirk of extract(), not something this
        # command should silently paper over by guessing a different path.
        # Running the whole extract+pack pass with CWD inside our own
        # scratch dir keeps that output fully contained here instead of
        # littering wherever the player happened to run this from.
        # This project's own custom overrides (e.g. the naming screen's
        # "Don't Care" name list, enemy name overrides) -- resolved before
        # the chdir below so a relative src/custom_assets still works.
        # Bundled alongside earthbound.yml in a frozen standalone
        # executable, same resolution order.
        real_custom_dir = _default_config_path("custom_assets", "src/custom_assets").resolve()

        prev_cwd = os.getcwd()
        os.chdir(scratch)
        try:
            print("Reading your ROM...")
            extract(
                yaml_config=yaml_config,
                rom_path=rom_path,
                commondata=commondata,
                dump_path=str(bin_dir),
            )
            asset_output_dir = scratch / "port" / "assets"

            # pack_all() looks for this project's custom overrides at a
            # hardcoded sibling of assets_dir ("assets_dir.parent /
            # custom_assets" -- see its dont_care_names/enemy_name_overrides
            # handling), not a parameter we can just pass in. Mirror that
            # layout here so those overrides are actually found instead of
            # silently falling back to the ROM's un-personalized defaults.
            if real_custom_dir.is_dir():
                shutil.copytree(real_custom_dir, asset_output_dir.parent / "custom_assets")

            # pack-all also (re-)generates text_refs.h, a C header for
            # rebuilding the game from source -- irrelevant to a one-time
            # pak build, so point it at a throwaway scratch spot instead of
            # its "assets_dir/../data" default (which only makes sense for
            # a real source checkout, not this scratch layout).
            text_refs_scratch = scratch / "text_refs_scratch"
            text_refs_scratch.mkdir(parents=True, exist_ok=True)

            print("Packing assets...")
            pack_all(
                assets_dir=asset_output_dir,
                output_dir=packed_dir,
                bin_dir=bin_dir,
                yaml_config=yaml_config,
                commondata=commondata,
                text_refs_dir=text_refs_scratch,
            )
        finally:
            os.chdir(prev_cwd)

        print("Building assets.pak...")
        manifest = bin_dir / "assets.manifest"
        out.parent.mkdir(parents=True, exist_ok=True)
        # packed_dir (pack-all's output, just built above) is the override
        # layer pack_assets() needs -- it already incorporates the
        # custom_assets overrides copied in above, so this is the ONE
        # custom_dir this call needs (not real_custom_dir directly, which
        # only has the 1-2 small override JSON files, not the full
        # packed/repacked binary output pack_assets() actually reads).
        pack_assets(
            manifest,
            bin_dir,
            out,
            custom_dir=packed_dir,
            exclude=list(DEFAULT_ASSET_EXCLUDES),
            locale=locale,
        )

    print(f"Done! Wrote {out}")
    print("You can launch the game now.")

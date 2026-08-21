"""Where assets.pak lives -- mirrors src/data/runtime_assets.c's
eb_runtime_assets_default_path() exactly. Keep both in sync."""

import os
from pathlib import Path


def default_assets_pak_path() -> Path:
    if os.name == "nt":
        appdata = os.environ.get("APPDATA")
        if not appdata:
            return Path("assets.pak")
        return Path(appdata) / "EarthBoundRecomp" / "assets.pak"

    data_home = os.environ.get("XDG_DATA_HOME")
    if not data_home:
        home = os.environ.get("HOME")
        if not home:
            return Path("assets.pak")
        data_home = str(Path(home) / ".local" / "share")
    return Path(data_home) / "EarthBoundRecomp" / "assets.pak"

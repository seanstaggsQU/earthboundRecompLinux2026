"""PyInstaller entry point for the standalone ebtools-setup helper.

Bundled alongside the game binary in a release so the game can shell out to
it silently on first launch (see src/data/rom_extract.c). Behaves exactly
like `ebtools setup <rom> [--out PATH]` -- this file just gives PyInstaller
a plain script to build from, since the bundle's whole purpose is *being*
the setup command (no other subcommands needed here).
"""

from cyclopts import App

from ebtools.cli.setup import setup

if __name__ == "__main__":
    app = App(help="Build assets.pak from your own EarthBound ROM.")
    app.default(setup)
    app()

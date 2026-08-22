# Getting Your Own Game Data

EarthBound's actual game data (the graphics, music, text, all of it) belongs to
Nintendo, not us — so instead of shipping you a copy, this game builds what it
needs straight from your own ROM, the first time you run it. This page covers
that, plus adding an MSU audio pack if you want higher-quality music.

## Setting up the game

1. Get your own EarthBound (USA) ROM. However you already have one is fine —
   we don't care about the filename, just that it's the real thing.
2. Put it in the same folder as the game (right next to the `earthbound` /
   `earthbound.exe` binary). Rename it or don't, doesn't matter — anything
   ending in `.sfc` or `.smc` works.
3. Launch the game.

That's it. The first launch checks your ROM, then shows a small "Setting Up"
window while it builds what it needs (usually well under a minute) — nothing
to click through, it closes on its own. Every launch after that goes straight
to the title screen, since it only needs to do this once.

If something's off — no ROM found, or the file doesn't check out as a real
EarthBound ROM — you'll get a small popup telling you so, and the game will
close. Just fix whatever it's complaining about and try again.

Where the built game data actually lands, if you're curious:
- **macOS/Linux**: `~/.local/share/EarthBoundRecomp/assets.pak` (or
  `$XDG_DATA_HOME/EarthBoundRecomp/assets.pak` if you've set that)
- **Windows**: `%APPDATA%\EarthBoundRecomp\assets.pak`

You can also point the game at a specific ROM or pak file yourself if you'd
rather not drop it next to the binary — see `./earthbound --help` for the
`--assets`/`--verify` flags.

## Adding an MSU audio pack (optional)

If you have an MSU-1 audio pack for EarthBound (community-made, replaces the
in-game music with studio-quality recordings), drop its `.pcm` files into a
folder named `msu` next to the game binary. The game finds it automatically —
no setup, no flags, no renaming needed. If you don't have one or don't care,
skip this entirely; the game sounds exactly like the original either way.

## Why it works this way

The game ships with a small bundled helper (`ebtools-setup`, sitting right
next to the game binary) that does the actual building -- it's the exact
same tooling used to build the game itself, so you get the exact same
result. You never interact with it directly; the game just runs it quietly
in the background the first time it needs to. No Python or extra install
required on your end, it's fully self-contained.

If you're curious about the maintainer-side tooling behind this (or you're
building from source and want the old compile-everything-in build instead),
see [docs/assets.md](assets.md).

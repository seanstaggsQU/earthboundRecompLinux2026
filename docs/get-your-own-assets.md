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

That's it. The first launch quietly checks your ROM, builds what it needs, and
starts playing — no popup, no progress bar, nothing to click through. Every
launch after that just works, since it only needs to do this once.

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

Building the game's data is just slicing bytes straight out of your ROM at
known locations — no decompression, no image conversion, nothing fancy — so
the game can do it itself in well under a second, in C, with no extra tools
or Python needed on your end. If you're curious about the maintainer-side
tooling behind this (or you're building from source and want the old
compile-everything-in build instead), see [docs/assets.md](assets.md).

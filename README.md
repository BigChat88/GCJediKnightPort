# Jedi Knight for GameCube

A port of **Star Wars Jedi Knight: Dark Forces II** and its expansion
**Mysteries of the Sith** to the Nintendo GameCube, built on
[OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2) (the open-source
reimplementation of the Jedi Knight engine) and devkitPPC/libogc, plus a tool
that turns your own copy of the games into bootable disc images.

No copyrighted game data is in this repository. You supply your own CD images;
the build packs them into the disc image.

## What you need to play

* A GameCube (or Wii in GameCube mode) that runs homebrew: **Swiss** with an SD
  card adapter (SD2SP2, SD Gecko), or an optical drive emulator. Dolphin works too.
* A **memory card** in slot A or B for settings and saved games (settings take
  2 blocks, each saved game about 5-7).
* The game looks for its image on the SD card at startup (`sd:/openjkdf2.iso`,
  anything under `sd:/games/`, or `.iso` files in the root); FAT and exFAT cards
  both work.
* With a component/digital cable it runs in 480p. If the screen stays blank, hold
  **B** while it boots for 480i; Setup > Display changes it later.

## The game files you need

Your own CDs of **Jedi Knight** (both discs) and, if you want it,
**Mysteries of the Sith**, ripped as `.bin`/`.cue` ("one file per track", which
keeps the CD audio soundtrack). See [gamedata/README.md](gamedata/README.md).

## How to build the disc images

You only need **[Python 3](https://www.python.org/downloads/)** on your `PATH`.
No Docker or GameCube toolchain.

1. Grab the latest [Release](../../releases) (`GCJediKnight-x.y.z.zip`) and unzip it.
2. Put your CD images in `gamedata/`.
3. Build:
   * **Windows:** double-click `build.cmd`.
   * **Linux/macOS:** `./build.sh`
4. Wait for `OK -> output/openjkdf2.iso` (and `output/openjkm.iso` for Mysteries
   of the Sith). Copy them to your SD card (e.g. `sd:/games/`) and boot them
   from Swiss, or open them in Dolphin.

The first run installs `numpy` (soundtrack conversion) and `Pillow` (disc banner)
with pip. `python tools/pack_iso.py --help` lists the options (other folders,
one game only, no music).

## Controls

| Button | Action |
|---|---|
| Main stick | Move / strafe |
| C-stick | Aim |
| R | Fire |
| Z | Secondary fire |
| A | Jump |
| X | Crouch |
| B | Activate (doors, switches) |
| Y | Use inventory item |
| L | Use Force power |
| D-pad left/right | Previous / next weapon |
| D-pad up / down | Next Force power / next item |
| START | Pause menu (map, save, load, setup) |

In menus: stick or D-pad to move, A to accept, B to go back.

## Building from source

The engine needs devkitPPC; the Docker image in `OpenJKDF2/packaging/gamecube`
has it:

```sh
docker build -t openjkdf2-gamecube OpenJKDF2/packaging/gamecube
docker run --rm -v "$PWD/OpenJKDF2:/src" -w /src openjkdf2-gamecube ./build_gamecube.sh
python tools/pack_iso.py      # uses OpenJKDF2/build_gamecube/ when there is no engine/
```

`OpenJKDF2/packaging/gamecube/README.md` documents the port: disc layout,
load-time work (GOB ordering, the ARAM read cache), memory card saves, video
modes and the headless Dolphin test image. `dev.sh` is the build-and-boot loop
used during development (needs both Docker images).

## Versioning and releases

`VERSION` holds the version (semver). It is compiled into the engine (printed at
boot) and names the release. Every push to `main` builds the engine on GitHub
Actions (`.github/workflows/release.yml`) and publishes the `vX.Y.Z` pre-release
with the bundle and a `-debug.zip` (`.elf` + `.map`, to symbolicate crash dumps
from real hardware). Pull requests only build. Bump `VERSION` for each release;
a push without a bump refreshes the existing release.

## Credits

* [OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2) by shinyquagsire23
  and contributors -- the engine this port is built on.
* [devkitPro](https://devkitpro.org/) -- devkitPPC, libogc and libfat.
* Star Wars Jedi Knight: Dark Forces II and Mysteries of the Sith (C) Lucasfilm
  Ltd. This is an unofficial fan project, not affiliated with or endorsed by
  Lucasfilm or Disney. See [LICENSE](LICENSE).

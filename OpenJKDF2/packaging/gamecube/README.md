# OpenJKDF2 for the Nintendo GameCube

Work-in-progress port (devkitPPC + libogc2). Platform code lives in
`src/Platform/GameCube/`; the build target is `PLAT_GAMECUBE`.

## Building

```sh
docker build -t openjkdf2-gamecube packaging/gamecube
docker run --rm -v "$PWD:/src" -w /src openjkdf2-gamecube ./build_gamecube.sh
```

Output: `build_gamecube/openjkdf2.dol` and `build_gamecube/apploader.img`.

Configure-time options (environment variables):

| Variable | Effect |
|---|---|
| `GC_SKIP_CUTSCENES=1` | Cutscenes off by default (faster boot tests) |
| `GC_AUTOBOOT_MAP=01narshadda.jkl` | Boot straight into a level |
| `GC_TRACE=1` | Diagnostic build: logs the main thread's call stack every 5 s |
| `GC_BOOT_CONSOLE=1` | Startup log, SD card details and cutscene timings on screen (diagnosing boots on real hardware) |
| `GC_INPUT_SCRIPT=20000:A,25000:DOWN` | Scripted controller presses (ms since boot) for automated tests |

## Game data and disc image

Game assets are not included. Lay out your own CD data (raw `.bin` data tracks
or `.iso`) and build a GameCube disc image:

```sh
python packaging/gamecube/extract_assets.py "Disc 1/Track 01.bin" "Disc 2/Track 01.bin" -o disc
python packaging/gamecube/make_gcm.py build_gamecube/openjkdf2.dol disc -o openjkdf2.iso --apploader build_gamecube/apploader.img
```

The engine reads everything from `dvd:/jk1/`, and the soundtrack from `dvd:/music/`
(converted from the CD audio tracks; needs numpy).

`make_gcm.py` also writes `/opening.bnr` (the 96x32 banner plus name, maker and
description shown by Swiss and the IPL) from `packaging/gamecube/banner.png`
(any image works, it is scaled; `--banner` picks another one). The texts live in
`BANNER_TEXT` in the script.

The image boots on its own (Dolphin, Swiss, ODEs) through the apploader in
`packaging/gamecube/apploader/`. For quick iteration you can also boot a fresh
`.dol` directly with the image set as Dolphin's *Default ISO* (Config > Paths).

### Real hardware (Swiss)

Booted from one of its own images (a burned disc, an optical drive emulator, a
loader such as cubeboot), the game reads it through the drive; it checks that the
drive really serves the image's data (its first and last GOB). Swiss's disc
emulation only hooks the official Nintendo SDK's drive routines, so an image Swiss
boots from an SD card fails that check, and the game then looks for its own image
on the SD card (SD2SP2, then SD Gecko style adapters in slot B and slot A):
`sd:/openjkdf2.iso`, files named `openjk*` in the root or under `sd:/games/` (two
levels deep) first, then any `.iso`/`.gcm`, recognized by the disc ID `OJKEOJ`
(`OJMEOJ` for Mysteries of the Sith). With no image anywhere it waits at most 15 s
for a drive before showing an error.

The SD card is mounted with libogc2's libdvm: FAT and exFAT (what large cards
usually use) alike.

Copy `openjkdf2.iso` to the card, then boot either the ISO or `openjkdf2.dol` from
Swiss. Saves still go to the memory card in slot A. If it hangs on hardware, a
`GC_BOOT_CONSOLE=1` build shows the startup log on screen, so you can see where it
stopped.

## Load times

Disc seeks (~100 ms each) dominate level loads, so the disc is prepared to avoid
them:

- `extract_assets.py` rewrites the GOBs in the order levels access their files
  (`gob_order.txt`), so a level load is mostly sequential reads.
- It also writes `resource/wavinfo.txt` with every WAV header, so loading a level
  doesn't read each sound file (the PCM is read on first play).
- At runtime `gcDvd` keeps a 13 MiB read cache in ARAM, filled in 256 KiB slices.

`gob_order.txt` holds file names only. Regenerate it with `record_access.py` (boots
every level in the headless Dolphin with a `GC_LOG_GOB_OPENS=1
GC_AUTOBOOT_FROM_DISC=1` build and an image made with `make_gcm.py --autoboot-slot`).

## Automated boot test (headless Dolphin)

```sh
docker build -t openjkdf2-dolphin packaging/gamecube/test
docker run --rm --shm-size=1g -v "<dir>:/work" openjkdf2-dolphin /work/openjkdf2.iso /work/openjkdf2.iso 60
```

(Dolphin 2609: it boots disc images only, so pack the DOL into one first with
`make_gcm.py`; `dev.sh` at the repository root does it all. Sound needs
`-e DOLPHIN_ARGS="-C Dolphin.Core.DSPHLE=False"` -- DSP LLE.)

It prints the OSReport log, and frame dumps (PNG) land in `<dir>/frames`.
Pass `-e DOLPHIN_ARGS="-C Dolphin.Core.FastDiscSpeed=True"` to skip DVD timing
emulation.

## Menus and saving

The console build has a single, fixed profile ("Kyle", created on the first boot)
and a flat main menu: Single Player (pick a difficulty, the episode starts), Load
Game, Setup, Cutscenes, Credits. No multiplayer, no player names to type.

Memory card (slot A or B):

* `openjkdf2_a.sav` / `openjkdf2_b.sav` -- settings (profile, options, controls,
  level progress), 1 block each, written alternately so a power cut mid-write
  only loses the last change.
* `openjkdf2_s1.sav` .. `_s3.sav` -- saved games, one slot per file, zlib
  compressed (a ~380 KiB save takes ~5 blocks). Saving offers the used slots plus
  one empty slot when the card has room; slots are labelled with their level.

The level-start autosave (restart after dying) stays in RAM. Every card file
carries a banner and an icon for the GameCube's memory card screen and Swiss:
`packaging/gamecube/make_card_gfx.py` turns `card_banner.png` and
`card_icon.png` into `src/Platform/GameCube/gcCardGfx.h`. The disc banner
(`opening.bnr`, shown by Swiss and the GameCube menu) comes from `banner.png`.
`prepare_art.py` makes all three from full-size artwork, keeping the aspect
ratio. See
`src/Platform/GameCube/gcCard.c`.

## Video mode

With a component/digital cable the game runs in 480p, otherwise 480i. Hold B
while booting to get 480i anyway (for screens that can't show 480p); Setup >
Display switches it on the spot. The choice is kept in the card settings.

Setup pages on the console keep their changes when you go back (B); there is no
Cancel. Options that don't apply here are hidden: keyboard/mouse/joystick pages
and control config files, the multiplayer column of Gameplay, 3D sound and the
sound resolution note. Leaving Setup writes the settings to the card. PAL consoles get 480p at 60 Hz. See
`gcSystem_ChooseVideoMode` in `src/Platform/GameCube/gcSystem.c`.

## Status

Working: boot (the image boots on its own via the bundled apploader), disc
filesystem, GOB/BM/SFT/CMP/MAT/WAV loading on big-endian, GUI and HUD, Smacker
cutscenes with audio, GX renderer (CI8 textures + TLUT, perspective-correct, depth),
controller input, level loading, sound effects (AESND), streamed CD soundtrack,
settings and saved games on the memory card, tested on real hardware (Swiss).

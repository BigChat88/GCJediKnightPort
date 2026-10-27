#!/usr/bin/env python3
"""Build the GameCube disc images from your own copy of the games.

Finds the CD images in gamedata/ (or the folders given on the command line),
tells Jedi Knight discs from Mysteries of the Sith by their contents, lays the
data out for the GameCube (extract_assets.py), builds the sound bank
(make_sndbank.py) and writes the bootable images (make_gcm.py):

    output/openjkdf2.iso   Jedi Knight: Dark Forces II
    output/openjkm.iso     Mysteries of the Sith

    python tools/pack_iso.py                      # everything found in gamedata/
    python tools/pack_iso.py --gamedata D:/rips   # look somewhere else
    python tools/pack_iso.py --only jk            # just one game

A CD image is a data track (.bin with its .cue, as ripped "one file per track",
or a plain .iso). Keep each disc in its own folder: the .cue next to the data
track is how the CD audio tracks (the soundtrack) are found.

Optional: a JkGfxMod material pack with lightsaber blades (jkgm/materials/.../
metadata.json replacing mat/saber*.mat, e.g. "JK Neural Upscale Special Effect
Annex") anywhere in those folders: its blades are baked into the images
(make_saber_mats.py).

The engine comes from engine/ (release bundle) or, in a source checkout,
OpenJKDF2/build_gamecube/ (build it first, see README.md).

The sound bank (resource/sndbank.bin: every sound pre-encoded for the GameCube's
DSP) is made by a small C tool. The release bundle carries it built for Windows
and Linux in tools/bin/; elsewhere it is compiled with the system's C compiler
(cc/gcc/clang). Without it the game still works, but plays its sounds from the
WAVs in the GOBs: more memory used in the levels, and disc reads while playing.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGING = os.path.join(ROOT, "OpenJKDF2", "packaging", "gamecube")
sys.path.insert(0, PACKAGING)
import extract_assets  # noqa: E402  (CdImage, iso_walk)

GAMES = {
    "jk": {"title": "Jedi Knight: Dark Forces II", "iso": "openjkdf2.iso", "args": []},
    "mots": {"title": "Mysteries of the Sith", "iso": "openjkm.iso", "args": ["--mots"]},
}


def find_engine():
    for folder in (os.path.join(ROOT, "engine"), os.path.join(ROOT, "OpenJKDF2", "build_gamecube")):
        dol = os.path.join(folder, "openjkdf2.dol")
        apploader = os.path.join(folder, "apploader.img")
        if os.path.exists(dol) and os.path.exists(apploader):
            return dol, apploader
    sys.exit("error: no engine found. Use a release bundle (engine/ folder) or build it first "
             "(see README.md, 'Building from source').")


def data_tracks(folder):
    """Every disc's data track under `folder`: the first FILE of each .cue, or the .iso images
    in folders without a .cue. Sorted by path, so "Disc 1" comes before "Disc 2"."""
    found = []
    for dirpath, _, files in os.walk(folder):
        cues = sorted(f for f in files if f.lower().endswith(".cue"))
        for cue in cues:
            for line in open(os.path.join(dirpath, cue), encoding="latin1"):
                tok = line.strip().split('"')
                if line.strip().upper().startswith("FILE") and len(tok) >= 2:
                    path = os.path.join(dirpath, tok[1])
                    if os.path.exists(path):
                        found.append(path)
                    break
        if not cues:
            found += [os.path.join(dirpath, f) for f in sorted(files) if f.lower().endswith(".iso")]
    return sorted(found)


def identify(path):
    """'jk', 'mots' or None, from the files on the disc."""
    try:
        names = {p.upper() for p, _, _ in extract_assets.iso_walk(extract_assets.CdImage(path))}
    except Exception:
        return None
    if any(n.endswith(".GOO") for n in names):
        return "mots"
    if any(n.endswith(".GOB") or n.endswith("JK_.CD") for n in names):
        return "jk"
    return None


def find_sndbank_encoder(work_root):
    """The sound bank encoder: the one in tools/bin/ for this system, else compiled
    from OpenJKDF2/packaging/gamecube/sndbank/ with the C compiler. None if neither."""
    bindir = os.path.join(ROOT, "tools", "bin")
    machine = platform.machine().lower()
    if os.name == "nt":
        candidates = ["sndbank-windows-x86_64.exe"]
    elif sys.platform.startswith("linux") and machine in ("x86_64", "amd64"):
        candidates = ["sndbank-linux-x86_64"]
    else:
        candidates = []
    for name in candidates:
        path = os.path.join(bindir, name)
        if os.path.exists(path):
            return path

    src = os.path.join(PACKAGING, "sndbank")
    exe = os.path.join(work_root, "sndbank" + (".exe" if os.name == "nt" else ""))
    if os.path.exists(exe):
        return exe
    for cc in (os.environ.get("CC"), "cc", "gcc", "clang"):
        if cc and shutil.which(cc):
            os.makedirs(work_root, exist_ok=True)
            cmd = [cc, "-O2", "-o", exe, os.path.join(src, "sndbank.c"), os.path.join(src, "grok.c"), "-lm"]
            if run(cmd) == 0 and os.path.exists(exe):
                return exe
    return None


def build_sndbank(game_dir, work_root, clean):
    """resource/sndbank.bin for an extracted game; a warning (not an error) if it can't."""
    bank = os.path.join(game_dir, "resource", "sndbank.bin")
    if os.path.exists(bank) and not clean:
        print(f"  {os.path.relpath(bank, ROOT)} already built")
        return
    encoder = find_sndbank_encoder(work_root)
    if not encoder:
        print("warning: no sound bank encoder for this system and no C compiler found; the disc "
              "image is built without resource/sndbank.bin (sounds then play from the GOBs, "
              "which uses more memory in the levels)")
        return
    if run([sys.executable, os.path.join(PACKAGING, "make_sndbank.py"), game_dir, encoder]) != 0:
        if os.path.exists(bank):
            os.remove(bank)  # a half-written bank would be worse than none
        print("warning: building the sound bank failed; continuing without it")


def run(cmd):
    print("> " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    return subprocess.call(cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedata", nargs="+", default=[os.path.join(ROOT, "gamedata")],
                    help="folders to search for the CD images (default: gamedata/)")
    ap.add_argument("--out", default=os.path.join(ROOT, "output"), help="where the .iso files go")
    ap.add_argument("--only", choices=sorted(GAMES), help="build just this game")
    ap.add_argument("--no-music", action="store_true", help="skip the CD soundtrack (no numpy needed)")
    ap.add_argument("--clean", action="store_true", help="redo the extraction from scratch")
    args = ap.parse_args()

    dol, apploader = find_engine()
    discs = {key: [] for key in GAMES}
    for folder in args.gamedata:
        for track in data_tracks(folder):
            game = identify(track)
            if game:
                discs[game].append(track)
                print(f"found {GAMES[game]['title']} disc: {os.path.relpath(track, ROOT)}")
    wanted = [args.only] if args.only else [k for k in GAMES if discs[k]]
    if not wanted or not any(discs[k] for k in wanted):
        sys.exit("error: no Jedi Knight or Mysteries of the Sith CD images found in "
                 + ", ".join(args.gamedata) + " (see gamedata/README.md)")

    os.makedirs(args.out, exist_ok=True)
    failed = []
    for key in wanted:
        game = GAMES[key]
        if not discs[key]:
            print(f"\n{game['title']}: no discs found, skipped")
            failed.append(key)
            continue
        print(f"\n=== {game['title']} ===")
        work = os.path.join(args.out, "work", key)
        if args.clean and os.path.exists(work):
            shutil.rmtree(work)
        extract = [sys.executable, os.path.join(PACKAGING, "extract_assets.py"), *discs[key], "-o", work, *game["args"]]
        if args.no_music:
            extract.append("--no-music")
        iso = os.path.join(args.out, game["iso"])
        if run(extract) != 0:
            failed.append(key)
            continue
        game_dir = os.path.join(work, "mots" if key == "mots" else "jk1")
        build_sndbank(game_dir, os.path.join(args.out, "work"), args.clean)
        # Lightsaber blades from a JkGfxMod pack in the same folders, if there is one.
        if run([sys.executable, os.path.join(PACKAGING, "make_saber_mats.py"), game_dir, *args.gamedata]) != 0:
            print("warning: the saber blades from the JkGfxMod pack could not be made; "
                  "the game's own are used")
        if run([sys.executable, os.path.join(PACKAGING, "make_gcm.py"), dol, work,
                "-o", iso, "--apploader", apploader, *game["args"]]) != 0:
            failed.append(key)
            continue
        print(f"OK -> {os.path.relpath(iso, ROOT)} ({os.path.getsize(iso) / 2**20:.0f} MiB)")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

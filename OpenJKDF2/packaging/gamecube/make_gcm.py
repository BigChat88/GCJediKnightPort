#!/usr/bin/env python3
"""Build a GameCube disc image (GCM/ISO) from a .dol and a directory of files.

    python make_gcm.py openjkdf2.dol disc/ -o openjkdf2.iso [--apploader apploader.img]

Layout: boot.bin (0x440) + bi2.bin (0x2000) + apploader at 0x2440, then the main
DOL, the FST and the file data. Files are 2 KiB aligned so the engine's DVD reads
can DMA straight into its buffers.

The apploader is only executed when the disc itself is booted through the IPL/BS2
(real hardware, Dolphin booting the .iso). Without --apploader a placeholder is
written; boot the .dol directly in Dolphin with this image inserted as the disc
(Dolphin's "Default ISO"), which never runs it.
"""

import argparse
import os
import struct
import sys
import tempfile

GAME_ID = b"OJKE"       # O(penJK) JK E(nglish/US)
MAKER = b"OJ"
GAME_NAME = b"Star Wars Jedi Knight: Dark Forces II"
MAX_DISC_SIZE = 1459978240
FILE_ALIGN = 2048
REGION_NTSC_U = 1


def align(n, a):
    return (n + a - 1) & ~(a - 1)


def collect(root):
    """Return a nested tree {name: subtree or file path}, sorted like the FST wants."""
    tree = {}
    for name in sorted(os.listdir(root), key=lambda s: s.lower()):
        path = os.path.join(root, name)
        if os.path.isdir(path):
            tree[name] = collect(path)
        else:
            tree[name] = path
    return tree


def build_fst(tree, data_start):
    entries = []      # [isDir, nameOff, offset/parent, length/next]
    names = bytearray()
    files = []        # (disc offset, host path)
    cursor = [data_start]

    def add_name(n):
        off = len(names)
        names.extend(n.encode("latin1") + b"\x00")
        return off

    entries.append([1, 0, 0, 0])  # root
    names.extend(b"\x00")

    def walk(node, parent_idx):
        for name, child in node.items():
            if isinstance(child, dict):
                idx = len(entries)
                entries.append([1, add_name(name), parent_idx, 0])
                walk(child, idx)
                entries[idx][3] = len(entries)
            else:
                size = os.path.getsize(child)
                off = align(cursor[0], FILE_ALIGN)
                entries.append([0, add_name(name), off, size])
                files.append((off, child))
                cursor[0] = off + size

    walk(tree, 0)
    entries[0][3] = len(entries)

    blob = bytearray()
    for is_dir, name_off, a, b in entries:
        blob += struct.pack(">III", (is_dir << 24) | name_off, a, b)
    blob += names
    return bytes(blob), files, cursor[0]


BANNER_W, BANNER_H = 96, 32
BANNER_TEXT = {
    # field: (size, text) -- BNR1 strings are ASCII/Shift-JIS, NUL padded.
    "short_name": (0x20, "Jedi Knight"),
    "short_maker": (0x20, "OpenJKDF2"),
    "long_name": (0x40, "Star Wars Jedi Knight: Dark Forces II"),
    "long_maker": (0x40, "LucasArts / OpenJKDF2 port"),
    "description": (0x80, "Kyle Katarn's fight against the Dark Jedi"),
}

# --mots: Mysteries of the Sith. Its own game ID, so the game finds its own image on
# an SD card and keeps its own memory card files (src/Platform/GameCube/gcGame.c).
MOTS_GAME_ID = b"OJME"
MOTS_GAME_NAME = b"Star Wars Jedi Knight: Mysteries of the Sith"
MOTS_BANNER_TEXT = {
    "short_name": (0x20, "Mysteries of the Sith"),
    "short_maker": (0x20, "OpenJKDF2"),
    "long_name": (0x40, "Star Wars Jedi Knight: Mysteries of the Sith"),
    "long_maker": (0x40, "LucasArts / OpenJKDF2 port"),
    "description": (0x80, "Mara Jade uncovers the secrets of the Sith"),
}


def make_banner(image_path, texts=None):
    """opening.bnr (BNR1): 96x32 RGB5A3 image + game name/maker/description.
    The IPL and loaders such as Swiss show it from the disc root."""
    from PIL import Image
    im = Image.open(image_path).convert("RGBA")
    if im.size != (BANNER_W, BANNER_H):
        im = im.resize((BANNER_W, BANNER_H), Image.LANCZOS)
    px = im.load()

    def rgb5a3(r, g, b, a):
        if a >= 0xE0:
            return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
        return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)

    bnr = bytearray(0x1960)
    bnr[0:4] = b"BNR1"
    off = 0x20
    for ty in range(0, BANNER_H, 4):          # 4x4 texel tiles
        for tx in range(0, BANNER_W, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    struct.pack_into(">H", bnr, off, rgb5a3(*px[x, y]))
                    off += 2
    off = 0x1820
    for size, text in (texts or BANNER_TEXT).values():
        raw = text.encode("ascii")[:size - 1]
        bnr[off:off + len(raw)] = raw
        off += size
    assert off == len(bnr)
    return bytes(bnr)


def placeholder_apploader():
    # Header only: date, entry point, size, trailer size. Never run when the .dol is
    # booted directly.
    hdr = bytearray(0x20)
    hdr[0:10] = b"2026/10/01"
    struct.pack_into(">III", hdr, 0x10, 0x81200000, 0, 0)
    return bytes(hdr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dol")
    ap.add_argument("root", help="directory that becomes the disc root (dvd:/)")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--apploader", help="apploader.img to embed")
    ap.add_argument("--autoboot-slot", action="store_true",
                    help="add a blank 64-byte /autoboot.txt (test harness, see record_access.py)")
    ap.add_argument("--banner", default=None,
                    help="image for /opening.bnr (scaled to 96x32; default: banner.png, or banner_mots.png "
                         "with --mots, next to this script)")
    ap.add_argument("--mots", action="store_true",
                    help="Mysteries of the Sith: its game ID, names and banner")
    args = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    if args.banner is None:
        args.banner = os.path.join(here, "banner_mots.png" if args.mots else "banner.png")
    game_id, game_name = (MOTS_GAME_ID, MOTS_GAME_NAME) if args.mots else (GAME_ID, GAME_NAME)

    dol = open(args.dol, "rb").read()
    # Tell the engine which game this image holds (gcGame_szImageTag): libogc
    # clears the disc ID in low memory at startup, so it can't read it from there.
    tag = b"GCGAMETAG:JK1:::"
    if dol.count(tag) != 1:
        sys.exit(f"error: {args.dol}: image tag not found (an engine older than this script?)")
    if args.mots:
        dol = dol.replace(tag, b"GCGAMETAG:JKM:::")
    apploader =open(args.apploader, "rb").read() if args.apploader else placeholder_apploader()

    dol_off = align(0x2440 + len(apploader), FILE_ALIGN)
    fst_off = align(dol_off + len(dol), FILE_ALIGN)

    tree = collect(args.root)
    if args.autoboot_slot:
        slot = os.path.join(tempfile.gettempdir(), "openjkdf2_autoboot.txt")
        with open(slot, "wb") as f:
            f.write(b" " * 64)
        tree["autoboot.txt"] = slot
    if args.banner and os.path.exists(args.banner):
        bnr = os.path.join(tempfile.gettempdir(), "openjkdf2_opening.bnr")
        with open(bnr, "wb") as f:
            f.write(make_banner(args.banner, MOTS_BANNER_TEXT if args.mots else BANNER_TEXT))
        tree["opening.bnr"] = bnr
    tree = dict(sorted(tree.items(), key=lambda kv: kv[0].lower()))
    # The FST size feeds the data start, so size it once with a dummy start.
    fst_probe, _, _ = build_fst(tree, 0)
    data_start = align(fst_off + len(fst_probe), FILE_ALIGN)
    fst, files, end = build_fst(tree, data_start)
    assert len(fst) == len(fst_probe)

    if end > MAX_DISC_SIZE:
        print(f"error: image would be {end / 1e6:.0f} MB, over the {MAX_DISC_SIZE / 1e6:.0f} MB "
              "GameCube disc limit", file=sys.stderr)
        return 1

    boot = bytearray(0x440)
    boot[0:4] = game_id
    boot[4:6] = MAKER
    struct.pack_into(">I", boot, 0x1C, 0xC2339F3D)
    boot[0x20:0x20 + len(game_name)] = game_name
    struct.pack_into(">IIII", boot, 0x420, dol_off, fst_off, len(fst), len(fst))

    # bi2.bin, as on retail discs. The IPL / Swiss take the console memory size from
    # "simulated memory size" when setting up the OS globals the DOL starts with.
    bi2 = bytearray(0x2000)
    struct.pack_into(">IIIIIIII", bi2, 0,
                     0,               # debug monitor size
                     0x01800000,      # simulated memory size (24 MiB)
                     0,               # argument offset
                     0,               # debug flag
                     0, 0,            # track location / size
                     REGION_NTSC_U,   # country code
                     1)               # (retail discs: 1)

    with open(args.out, "wb") as out:
        out.write(boot)
        out.write(bi2)
        out.write(apploader)
        out.seek(dol_off)
        out.write(dol)
        out.seek(fst_off)
        out.write(fst)
        for off, path in files:
            out.seek(off)
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(4 * 1024 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
        # Whole 32 KiB blocks: the disc cache reads in those units, and reading past
        # the last sector of a burned disc can fail.
        out.truncate(align(end, 0x8000))

    print(f"{args.out}: {len(files)} files, {end / 1e6:.1f} MB "
          f"(dol @0x{dol_off:x}, fst @0x{fst_off:x} {len(fst)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

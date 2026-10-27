#!/usr/bin/env python3
"""Lightsaber blades from a JkGfxMod material pack, for the GameCube.

JkGfxMod packs (e.g. "JK Neural Upscale Special Effect Annex") replace game
materials at draw time with PNGs: an albedo map plus an emissive map scaled by a
per-material colour factor, then tone mapped. The GameCube engine has no
JkGfxMod, so the saber ones are baked here into ordinary materials: for each
material a pack replaces among mat/saber*.mat, the texture is

    albedo * albedo alpha * albedo_factor  +  emissive * emissive_factor * EMISSIVE_SCALE

clamped, written as a 16-bit (RGB565) .mat into the game's mat/ folder, where a
loose file wins over the one in the GOBs. The texture header carries a marker
(GC_GLOW_MARKER) that tells the engine to draw the blade additively and unlit,
as JkGfxMod treats emissive light: the black around the glow adds nothing.

    make_saber_mats.py <game dir> <folder to search> [...]

Without such a pack nothing is written, and blades made by an earlier run are
removed (the list is kept in mat/gcsaber.txt).
"""

import json
import os
import struct
import sys

# The emissive factors in the packs are meant for HDR + tone mapping; taken as
# is they saturate the whole blade. This keeps a white core fading into colour.
EMISSIVE_SCALE = 0.35
GC_GLOW_MARKER = 0x47434C57  # "GCLW" in rdTextureHeader.unk_0c (see rdPolyline.c)
LIST_NAME = "gcsaber.txt"


def find_saber_entries(folders):
    """{'mat/sabergreen1.mat': (entry, base dir)} from every metadata.json found."""
    found = {}
    for folder in folders:
        for dirpath, _, files in os.walk(folder):
            if "metadata.json" not in files:
                continue
            try:
                with open(os.path.join(dirpath, "metadata.json"), encoding="utf-8") as f:
                    meta = json.load(f)
            except (OSError, ValueError):
                continue
            for entry in meta.get("materials", []):
                for repl in entry.get("replaces", []):
                    name = str(repl.get("name", "")).replace("\\", "/").lower()
                    if (repl.get("cel", 0) == 0 and name.startswith("mat/saber")
                            and name.endswith(".mat") and entry.get("albedo_map")):
                        found.setdefault(name, (entry, dirpath))
    return found


def compose(entry, base):
    """The baked texture: (width, height, [(r, g, b) floats 0-1])."""
    from PIL import Image
    albedo = Image.open(os.path.join(base, entry["albedo_map"])).convert("RGBA")
    emissive = None
    if entry.get("emissive_map"):
        emissive = Image.open(os.path.join(base, entry["emissive_map"])).convert("RGB")
        if emissive.size != albedo.size:
            emissive = emissive.resize(albedo.size, Image.BILINEAR)
    af = (entry.get("albedo_factor") or [1.0, 1.0, 1.0, 1.0])[:3]
    ef = entry.get("emissive_factor") or [0.0, 0.0, 0.0]
    w, h = albedo.size
    a_raw = albedo.tobytes()
    a_px = [tuple(a_raw[i:i + 4]) for i in range(0, len(a_raw), 4)]
    if emissive:
        e_raw = emissive.tobytes()
        e_px = [tuple(e_raw[i:i + 3]) for i in range(0, len(e_raw), 3)]
    else:
        e_px = [(0, 0, 0)] * (w * h)
    out = []
    for (r, g, b, a), (er, eg, eb) in zip(a_px, e_px):
        al = a / 255.0
        px = []
        for c, f, e, k in ((r, af[0], er, ef[0]), (g, af[1], eg, ef[1]), (b, af[2], eb, ef[2])):
            px.append(min(1.0, c / 255.0 * f * al + e / 255.0 * k * EMISSIVE_SCALE))
        out.append(px)
    return w, h, out


def write_mat(path, w, h, pixels):
    """A one-texture, one-cel 16-bit material (the game's own layout: see rdMaterial.c)."""
    fmt = struct.pack("<14I", 1, 16, 5, 6, 5, 11, 5, 0, 3, 2, 3, 0, 0, 0)
    texinfo = struct.pack("<II4f", 8, 0, 1.0, 1.0, 1.0, 1.0) + struct.pack("<4I", 0, 0, 0, 0)
    texhdr = struct.pack("<6I", w, h, 0, GC_GLOW_MARKER, 0, 1)
    data = bytearray()
    for r, g, b in pixels:
        v = (int(r * 31 + 0.5) << 11) | (int(g * 63 + 0.5) << 5) | int(b * 31 + 0.5)
        data += struct.pack("<H", v)
    with open(path, "wb") as f:
        f.write(b"MAT " + struct.pack("<IIII", 0x32, 2, 1, 1) + fmt + texinfo + texhdr + data)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    game_dir, folders = sys.argv[1], sys.argv[2:]
    mat_dir = os.path.join(game_dir, "mat")
    listing = os.path.join(mat_dir, LIST_NAME)

    # Blades from an earlier run go first (the pack may have been removed).
    if os.path.exists(listing):
        for name in open(listing, encoding="utf-8").read().split():
            p = os.path.join(mat_dir, name)
            if os.path.exists(p):
                os.remove(p)
        os.remove(listing)

    entries = find_saber_entries(folders)
    if not entries:
        return 0
    os.makedirs(mat_dir, exist_ok=True)
    written = []
    for name, (entry, base) in sorted(entries.items()):
        try:
            w, h, px = compose(entry, base)
        except (OSError, KeyError) as e:
            print(f"  saber: skipped {name} ({e})")
            continue
        fname = os.path.basename(name)
        write_mat(os.path.join(mat_dir, fname), w, h, px)
        written.append(fname)
    with open(listing, "w", encoding="utf-8") as f:
        f.write("\n".join(written) + "\n")
    print(f"saber: {len(written)} blade materials from a JkGfxMod pack -> {os.path.relpath(mat_dir)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

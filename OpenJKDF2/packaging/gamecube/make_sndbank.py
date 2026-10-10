#!/usr/bin/env python3
"""Build resource/sndbank.bin, the GameCube sound bank (DSP-ADPCM).

    make_sndbank.py <game dir> <sndbank encoder>

<game dir> is an extracted game folder (e.g. disc/jk1 or disc_mots/mots) with
resource/ and episode/ GOBs; <sndbank encoder> is the compiled
packaging/gamecube/sndbank tool. The bank is written to <game dir>/resource/.

Sounds are ordered by the first level that lists them (levels in name order), so a
level's sounds sit together and load with few seeks; see sndbank/sndbank.c.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile


def gob_entries(path):
    with open(path, "rb") as f:
        magic, _ver, diroff = struct.unpack("<4sII", f.read(12))
        if magic != b"GOB ":
            return []
        f.seek(diroff)
        (n,) = struct.unpack("<I", f.read(4))
        out = []
        for _ in range(n):
            off, size = struct.unpack("<II", f.read(8))
            name = f.read(128).split(b"\0")[0].decode("latin1")
            out.append((name, off, size))
        return out


def level_sound_names(path, off, size):
    with open(path, "rb") as f:
        f.seek(off)
        txt = f.read(size).decode("latin1")
    m = re.search(r"SECTION:\s*SOUNDS(.*?)SECTION:", txt, re.S | re.I)
    if not m:
        return []
    return [l.strip().lower() for l in m.group(1).splitlines() if l.strip().lower().endswith(".wav")]


def gobs_in(folder):
    if not os.path.isdir(folder):
        return []
    return [os.path.join(folder, g) for g in sorted(os.listdir(folder)) if g.lower().endswith((".gob", ".goo"))]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    game, encoder = sys.argv[1], sys.argv[2]
    # The episode's files take precedence over the shared resources.
    gobs = gobs_in(os.path.join(game, "episode")) + gobs_in(os.path.join(game, "resource"))
    if not gobs:
        sys.exit(f"make_sndbank: no GOBs in {game}")

    wavs = set()
    levels = []
    for gob in gobs:
        for name, off, size in gob_entries(gob):
            low = name.lower()
            if low.endswith(".wav"):
                wavs.add(low)
            elif low.endswith(".jkl"):
                levels.append((os.path.basename(low), gob, off, size))

    order = []
    seen = set()
    for _base, gob, off, size in sorted(levels, key=lambda l: (l[0] != "static.jkl", l[0])):
        for snd in level_sound_names(gob, off, size):
            for d in ("sound", "voice"):
                key = d + "\\" + snd
                if key in wavs and key not in seen:
                    seen.add(key)
                    order.append(key)
                    break

    out = os.path.join(game, "resource", "sndbank.bin")
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, newline="\n") as f:
        f.write("\n".join(order) + "\n")
        order_path = f.name
    try:
        return subprocess.call([encoder, out, order_path, *gobs])
    finally:
        os.unlink(order_path)


if __name__ == "__main__":
    sys.exit(main())

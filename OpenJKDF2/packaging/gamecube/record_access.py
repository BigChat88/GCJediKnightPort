#!/usr/bin/env python3
"""Record which GOB files each level loads, in order, to lay them out contiguously.

Boots every single-player level in the headless Dolphin test image with a dev build
(GC_LOG_GOB_OPENS=1 GC_AUTOBOOT_FROM_DISC=1) and collects the GOBOPEN log lines.
The image must contain a 64-byte /autoboot.txt (make_gcm.py --autoboot-slot),
which is patched in place before each run.

    python record_access.py openjkdf2.dol openjkdf2.iso -o gob_order.txt
    python record_access.py openjkdf2.dol openjkm.iso -o gob_order_mots.txt --mots

Output: one "<gob> <file>" line per file, static.jkl first, then levels in episode
order, each file listed once (first use). extract_assets.py --gob-order uses it.
"""

import argparse
import os
import re
import struct
import subprocess
import sys

LEVELS = [
    "01narshadda.jkl", "02narshadda.jkl", "03katarn.jkl", "04escapehouse.jkl",
    "06abarons.jkl", "06bbarons.jkl", "07yun.jkl", "08escape88.jkl",
    "09fuelstation.jkl", "10cargo.jkl", "11gorc.jkl", "12escape.jkl",
    "14tower.jkl", "15maw.jkl",
    # light side branch, then the dark side levels that differ
    "16aescapeship.jkl", "17asarris.jkl", "18ascend.jkl", "19a.jkl", "19b.jkl",
    "20aboc.jkl", "21ajerec.jkl",
    "16bescapeship.jkl", "17bsarris.jkl", "20bboc.jkl", "21bjerec.jkl",
]

# Mysteries of the Sith (episode jkm.goo), in episode order.
MOTS_LEVELS = [
    "s1l1_rebelbase.jkl", "s1l2_astcrust.jkl", "s1l3_astcore.jkl", "s1l4_astdestruct.jkl",
    "s2l1_palace.jkl", "s2l2a_katraasii.jkl", "s2l2b_katraasii.jkl", "s2l4_stronghold.jkl",
    "s4l1_escortship.jkl", "s4l2_shipyards.jkl", "s4l3_kaerobanibase.jkl",
    "s5l2_darkswamp.jkl", "s5l3_uppersith.jkl", "s5l4_lowersith.jkl",
]


def find_disc_file(iso, name):
    with open(iso, "rb") as f:
        hdr = f.read(0x440)
        fst_off, fst_size = struct.unpack(">II", hdr[0x424:0x42C])
        f.seek(fst_off)
        fst = f.read(fst_size)
    n = struct.unpack(">I", fst[8:12])[0]
    strs = fst[n * 12:]
    for i in range(1, n):
        tn, off, size = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        nm = strs[tn & 0xFFFFFF:strs.index(b"\0", tn & 0xFFFFFF)].decode()
        if tn >> 24 == 0 and nm.lower() == name:
            return off, size
    raise SystemExit(f"{name} not found in {iso} (build it with make_gcm.py --autoboot-slot)")


def run_level(work, dol, iso, secs):
    cmd = ["docker", "run", "--rm", "--shm-size=1g",
           "-e", "DOLPHIN_ARGS=-C Dolphin.Core.FastDiscSpeed=True",
           "-v", f"{work}:/work", "openjkdf2-dolphin",
           # The test image's Dolphin boots disc images only: the image (which
           # carries the dol) is both what's booted and the disc.
           f"/work/{iso}", f"/work/{iso}", str(secs)]
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    out = subprocess.run(cmd, capture_output=True, env=env).stdout.decode("utf-8", "replace")
    # Dolphin renders OSReport text as Shift-JIS, where 0x5C (backslash) is a yen sign.
    out = out.replace("¥", "\\")
    phase, opens = None, {}
    for line in out.splitlines():
        m = re.search(r"sithWorld_Load -> (\S+)", line)
        if m:
            phase = m.group(1).lower()
            opens.setdefault(phase, [])
            continue
        m = re.search(r"GOBOPEN (\S+) (\S+)", line)
        if m and phase:
            opens[phase].append((os.path.basename(m.group(1).replace("\\", "/")).lower(), m.group(2)))
    return opens


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dol", help="dev .dol, relative to --work")
    ap.add_argument("iso", help="image with an autoboot slot, relative to --work")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--work", default=".", help="directory mounted as /work")
    ap.add_argument("--secs", type=int, default=100, help="emulation time per level")
    ap.add_argument("--only", nargs="*", help="record just these levels")
    ap.add_argument("--mots", action="store_true", help="Mysteries of the Sith levels (an openjkm image)")
    args = ap.parse_args()

    iso_path = os.path.join(args.work, args.iso)
    slot_off, slot_size = find_disc_file(iso_path, "autoboot.txt")
    order, seen = [], set()

    def add(entries):
        for e in entries:
            if e not in seen:
                seen.add(e)
                order.append(e)

    for level in (args.only or (MOTS_LEVELS if args.mots else LEVELS)):
        with open(iso_path, "r+b") as f:
            f.seek(slot_off)
            f.write(level.encode().ljust(slot_size, b" "))
        opens = run_level(os.path.abspath(args.work), args.dol, args.iso, args.secs)
        add(opens.get("static.jkl", []))
        got = opens.get(level, [])
        add(got)
        print(f"{level}: {len(got)} opens, {len(order)} files so far", flush=True)

    with open(iso_path, "r+b") as f:
        f.seek(slot_off)
        f.write(b" " * slot_size)
    with open(args.out, "w", newline="\n", encoding="utf-8") as out:
        for gob, name in order:
            out.write(f"{gob} {name}\n")
    print(f"wrote {args.out}: {len(order)} files")


if __name__ == "__main__":
    sys.exit(main())

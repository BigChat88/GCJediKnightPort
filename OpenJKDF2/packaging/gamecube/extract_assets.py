#!/usr/bin/env python3
"""Lay out Jedi Knight: Dark Forces II (or, with --mots, Mysteries of the Sith) CD
data for the GameCube disc.

Reads the original CD data track(s) directly -- raw MODE1/2352 .bin images (as
ripped with their .cue) or plain 2048-byte .iso images -- and copies the game data
into the directory layout the GameCube build expects at dvd:/jk1/:

    <out>/jk1/resource/res1hi.gob, res2.gob, jk_.cd
    <out>/jk1/resource/video/*.smk
    <out>/jk1/episode/*.gob
    <out>/music/trackNN.pcm          (CD audio, 32 kHz 16-bit BE stereo; needs numpy)

Pass every disc you have, disc 1 first (they carry different cutscenes and music).

    python extract_assets.py "Disc 1/Track 01.bin" "Disc 2/Track 01.bin" -o disc

Mysteries of the Sith (one CD) goes to dvd:/mots/ instead, with its .goo
archives, SMUSH (.san) cutscenes and music tracks 2..14 as trackN.pcm:

    python extract_assets.py --mots "Track 01.bin" -o disc_mots
"""

import argparse
import os
import struct
import sys

SECTOR = 2048


class CdImage:
    def __init__(self, path):
        self.f = open(path, "rb")
        size = os.path.getsize(path)
        # Raw images start every sector with the 12-byte sync pattern.
        head = self.f.read(12)
        self.raw = head == b"\x00" + b"\xff" * 10 + b"\x00"
        self.stride = 2352 if self.raw else SECTOR
        self.offset = 16 if self.raw else 0
        if size % self.stride:
            print(f"warning: {path} size is not a multiple of {self.stride}", file=sys.stderr)

    def read(self, lba, length):
        out = bytearray()
        while len(out) < length:
            self.f.seek(lba * self.stride + self.offset)
            out += self.f.read(SECTOR)
            lba += 1
        return bytes(out[:length])


def iso_walk(img):
    """Yield (path, lba, size) for every file in the ISO9660 filesystem."""
    pvd = img.read(16, SECTOR)
    if pvd[1:6] != b"CD001":
        raise ValueError("no ISO9660 primary volume descriptor")
    root = pvd[156:156 + 34]

    def walk(lba, size, prefix):
        data = img.read(lba, size)
        pos = 0
        while pos < len(data):
            reclen = data[pos]
            if reclen == 0:
                # Records never straddle sectors; skip to the next one.
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + reclen]
            elba, esize = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            flags = rec[25]
            name = rec[33:33 + rec[32]]
            pos += reclen
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("latin1").split(";")[0].rstrip(".")
            path = f"{prefix}/{name}" if prefix else name
            if flags & 2:
                yield from walk(elba, esize, path)
            else:
                yield path, elba, esize

    rlba, rsize = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    yield from walk(rlba, rsize, "")


# CD path (upper case) -> destination under jk1/. Directories map recursively.
LAYOUT = {
    "GAMEDATA/RESOURCE/RES2.GOB": "resource/res2.gob",
    "GAMEDATA/RESOURCE/JK_.CD": "resource/jk_.cd",
    "GAMEDATA/MININSTALL/RES1HI.GOB": "resource/res1hi.gob",
    "GAMEDATA/RESOURCE/VIDEO/": "resource/video/",
    "GAMEDATA/EPISODE/": "episode/",
}


# Mysteries of the Sith -> mots/ (as OpenJKDF2's installer lays it out).
LAYOUT_MOTS = {
    "GAMEDATA/RESOURCE/JKMRES.GOO": "resource/jkmres.goo",
    "GAMEDATA/RESOURCE/JK_.CD": "resource/jk_.cd",
    "GAMEDATA/MININSTALL/JKMSNDLO.GOO": "resource/jkmsndlo.goo",
    "GAMEDATA/RESOURCE/VIDEO/": "resource/video/",
    "GAMEDATA/EPISODE/": "episode/",
}


def destination(cd_path, layout=None):
    up = cd_path.upper()
    if up.endswith(".ZIP"):
        return None  # MotS ships cutscenes.goo zipped; unpacked by main()
    for src, dst in (layout or LAYOUT).items():
        if src.endswith("/"):
            if up.startswith(src):
                return dst + cd_path[len(src):].lower()
        elif up == src:
            return dst
    return None


def gob_entries(path):
    """Yield (name, offset, size) for every file in a GOB."""
    with open(path, "rb") as f:
        hdr = f.read(12)
        if hdr[:4] != b"GOB ":
            return
        f.seek(struct.unpack_from("<I", hdr, 8)[0])
        count = struct.unpack("<I", f.read(4))[0]
        for _ in range(count):
            off, size = struct.unpack("<II", f.read(8))
            name = f.read(128).split(b"\0")[0].decode("latin1")
            yield name, off, size


def reorder_gob(path, order):
    """Rewrite a GOB so the files in `order` (lowercase names, access order) come
    first and contiguous; the rest keep their original order. Disc seeks dominate
    load times, so this turns a level load into mostly sequential reads."""
    entries = list(gob_entries(path))
    rank = {name: i for i, name in enumerate(order)}
    entries.sort(key=lambda e: (rank.get(e[0].lower(), len(rank)), ))
    tmp = path + ".tmp"
    with open(path, "rb") as src, open(tmp, "wb") as dst:
        dst.write(b"GOB " + struct.pack("<II", 0x14, 0))
        directory = []
        for name, off, size in entries:
            src.seek(off)
            new_off = dst.tell()
            dst.write(src.read(size))
            directory.append((name, new_off, size))
        dir_off = dst.tell()
        dst.write(struct.pack("<I", len(directory)))
        for name, off, size in directory:
            dst.write(struct.pack("<II", off, size) + name.encode("latin1").ljust(128, b"\0")[:128])
        dst.seek(8)
        dst.write(struct.pack("<I", dir_off))
    os.replace(tmp, path)
    return sum(1 for e in entries if e[0].lower() in rank)


def apply_gob_order(root, order_file):
    per_gob = {}
    for line in open(order_file, encoding="utf-8"):
        parts = line.strip().split(" ", 1)
        if len(parts) == 2:
            per_gob.setdefault(parts[0].lower(), []).append(parts[1].lower())
    for sub in ("resource", "episode"):
        folder = os.path.join(root, sub)
        for gob in sorted(os.listdir(folder)):
            if gob.lower() in per_gob:
                n = reorder_gob(os.path.join(folder, gob), per_gob[gob.lower()])
                print(f"  {sub}/{gob}: {n} files moved into level access order")


def parse_wav(data):
    """Mirror stdSound_ParseWav: (dataBytes, rate, bits, stereo, dataOffset) or None."""
    if data[8:12] != b"WAVE" or len(data) < 36:
        return None
    fmt_size = struct.unpack_from("<I", data, 16)[0]
    _, channels, rate, _, block_align, _ = struct.unpack_from("<hHIiHh", data, 20)
    bits = 8 * (block_align // channels) if channels else 0
    pos = 20 + 16
    if fmt_size > 16:
        pos += fmt_size - 16
    while pos + 8 <= len(data):
        chunk, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        pos += 8
        if chunk == b"data":
            return size, rate, bits, channels == 2, pos
        pos += size
    return None


def write_wav_info(root):
    """Pre-parse every WAV header in the GOBs into resource/wavinfo.txt, so the
    engine doesn't have to seek to each sound on the disc while loading a level."""
    lines = []
    for gob in sorted(os.listdir(os.path.join(root, "resource"))):
        if not gob.lower().endswith((".gob", ".goo")):
            continue
        path = os.path.join(root, "resource", gob)
        with open(path, "rb") as f:
            for name, off, size in gob_entries(path):
                if not name.lower().endswith(".wav"):
                    continue
                f.seek(off)
                info = parse_wav(f.read(min(size, 4096)))
                if info:
                    data_bytes, rate, bits, stereo, data_off = info
                    lines.append(f"{name.lower()} {rate} {bits} {int(stereo)} {data_off} {data_bytes}\n")
    with open(os.path.join(root, "resource", "wavinfo.txt"), "w", newline="\n") as out:
        out.write(f"WAVINFO {len(lines)}\n")
        out.writelines(lines)
    print(f"  resource/wavinfo.txt: {len(lines)} sounds")


def copy_file(img, lba, size, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst) and os.path.getsize(dst) == size:
        return False
    chunk = 256 * SECTOR
    with open(dst, "wb") as out:
        done = 0
        while done < size:
            n = min(chunk, size - done)
            out.write(img.read(lba + done // SECTOR, n))
            done += n
    return True


MUSIC_RATE = 32000      # must match GCMUSIC_RATE in src/Platform/GameCube/gcMusic.c
CDDA_RATE = 44100
CD_FRAME = 2352


def cue_audio_tracks(bin_path):
    """Yield (track number, .bin path, byte offset of INDEX 01) for the AUDIO tracks
    listed in the .cue next to a data track .bin (one file per track, Redump style)."""
    folder = os.path.dirname(bin_path)
    cues = [f for f in os.listdir(folder or ".") if f.lower().endswith(".cue")]
    if not cues:
        return
    cur_file = cur_track = None
    is_audio = False
    for line in open(os.path.join(folder, cues[0]), encoding="latin1"):
        tok = line.split()
        if not tok:
            continue
        if tok[0] == "FILE":
            cur_file = os.path.join(folder, line.split('"')[1])
        elif tok[0] == "TRACK":
            cur_track = int(tok[1])
            is_audio = tok[2] == "AUDIO"
        elif tok[0] == "INDEX" and tok[1] == "01" and is_audio:
            mm, ss, ff = (int(x) for x in tok[2].split(":"))
            yield cur_track, cur_file, ((mm * 60 + ss) * 75 + ff) * CD_FRAME


def convert_music(bin_path, disc_num, out_root):
    """CD-DA (44.1 kHz 16-bit LE stereo) -> MUSIC_RATE 16-bit BE stereo headerless PCM,
    named with the GOG track numbering the engine uses (disc * 10 + track)."""
    try:
        import numpy as np
    except ImportError:
        print("numpy not installed: skipping the soundtrack", file=sys.stderr)
        return 0
    os.makedirs(os.path.join(out_root, "music"), exist_ok=True)
    count = 0
    for track, path, offset in cue_audio_tracks(bin_path):
        dst = os.path.join(out_root, "music", f"track{disc_num * 10 + track}.pcm")
        if os.path.exists(dst):
            continue
        raw = np.fromfile(path, dtype="<i2", offset=offset)
        raw = raw[: len(raw) // 2 * 2].reshape(-1, 2).astype(np.float32)
        n_out = int(len(raw) * MUSIC_RATE / CDDA_RATE)
        t = np.arange(n_out) * (CDDA_RATE / MUSIC_RATE)
        src = np.arange(len(raw))
        out = np.empty((n_out, 2), dtype=np.float32)
        for ch in range(2):
            # light 3-tap low-pass before decimating, to tame aliasing
            x = raw[:, ch]
            x = np.convolve(x, [0.25, 0.5, 0.25], mode="same")
            out[:, ch] = np.interp(t, src, x)
        np.clip(out, -32768, 32767).astype(">i2").tofile(dst)
        count += 1
        print(f"  music: {os.path.basename(path)} -> music/{os.path.basename(dst)} "
              f"({n_out / MUSIC_RATE:.0f}s)")
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="CD data track images (.bin MODE1/2352 or .iso), disc 1 first")
    ap.add_argument("-o", "--out", required=True, help="output disc root directory")
    ap.add_argument("--no-music", action="store_true", help="skip converting the CD audio tracks")
    ap.add_argument("--mots", action="store_true", help="Mysteries of the Sith (into mots/)")
    ap.add_argument("--gob-order", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "gob_order.txt"),
                    help="recorded level access order (record_access.py); GOBs are rewritten to follow it")
    args = ap.parse_args()

    game_dir = "mots" if args.mots else "jk1"
    layout = LAYOUT_MOTS if args.mots else LAYOUT
    root = os.path.join(args.out, game_dir)
    copied = 0
    for disc_num, path in enumerate(args.images, start=1):
        if not args.no_music and path.lower().endswith(".bin"):
            # JK: GOG numbering (disc * 10 + track); MotS (one disc): the CD track number.
            convert_music(path, 0 if args.mots else disc_num, args.out)
        img = CdImage(path)
        print(f"{path}: {'raw 2352' if img.raw else 'iso 2048'} image")
        for cd_path, lba, size in iso_walk(img):
            if args.mots and cd_path.upper() == "GAMEDATA/RESOURCE/VIDEO/CUTSCENES.ZIP":
                import io
                import zipfile
                with zipfile.ZipFile(io.BytesIO(img.read(lba, size))) as z:
                    for info in z.infolist():
                        dst = os.path.join(root, "resource", "video", os.path.basename(info.filename).lower())
                        if not os.path.exists(dst) or os.path.getsize(dst) != info.file_size:
                            os.makedirs(os.path.dirname(dst), exist_ok=True)
                            with open(dst, "wb") as out:
                                out.write(z.read(info))
                            copied += 1
                            print(f"  {cd_path}:{info.filename} -> {game_dir}/resource/video/")
                continue
            rel = destination(cd_path, layout)
            if not rel:
                continue
            if copy_file(img, lba, size, os.path.join(root, rel)):
                copied += 1
                print(f"  {cd_path} -> {game_dir}/{rel} ({size / 1e6:.1f} MB)")

    required = (("resource/jkmres.goo", "resource/jk_.cd", "episode/jkm.goo") if args.mots else
                ("resource/res1hi.gob", "resource/res2.gob", "resource/jk_.cd", "episode/jk1.gob"))
    missing = [d for d in required if not os.path.exists(os.path.join(root, d))]
    if missing:
        print("missing required files: " + ", ".join(missing), file=sys.stderr)
        return 1
    # Mysteries of the Sith has its own recording (gob_order_mots.txt, next to it).
    order_file = args.gob_order
    if args.mots and order_file:
        order_file = os.path.join(os.path.dirname(order_file), "gob_order_mots.txt")
    if order_file and os.path.exists(order_file):
        apply_gob_order(root, order_file)
    write_wav_info(root)
    # Writable dirs the engine expects to exist (saves are redirected later).
    os.makedirs(os.path.join(root, "player"), exist_ok=True)
    print(f"done, {copied} files copied into {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

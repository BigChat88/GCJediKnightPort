#!/usr/bin/env python3
"""Turn full-size artwork into the small images the disc and memory card use,
keeping the aspect ratio (the logos are wider than the 3:1 banner slot):

    banner.png       96x32  disc banner (opening.bnr: Swiss, the GameCube menu)
    card_banner.png  96x32  memory card banner
    card_icon.png    32x32  memory card icon

    python prepare_art.py --disc-banner jd_banner.png --card-banner jd_cd.png --icon jd_icon.png

Then run make_card_gfx.py (card images) and rebuild the disc image (disc banner).
"""

import argparse
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def fit(path, w, h, background):
    """Scale to fit w x h, centred on `background` (RGBA)."""
    im = Image.open(path).convert("RGBA")
    bbox = im.getchannel("A").getbbox()
    if bbox:
        im = im.crop(bbox)  # drop fully transparent margins
    scale = min(w / im.width, h / im.height)
    sw, sh = max(1, round(im.width * scale)), max(1, round(im.height * scale))
    # Two steps keeps thin strokes from aliasing on big reductions.
    if im.width > 4 * sw:
        im = im.resize((sw * 4, sh * 4), Image.LANCZOS)
    im = im.resize((sw, sh), Image.LANCZOS)
    out = Image.new("RGBA", (w, h), background)
    out.alpha_composite(im, ((w - sw) // 2, (h - sh) // 2))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc-banner", required=True)
    ap.add_argument("--card-banner", required=True)
    ap.add_argument("--icon", required=True)
    ap.add_argument("--mots", action="store_true",
                    help="Mysteries of the Sith: write banner_mots.png, card_banner_mots.png, card_icon_mots.png")
    args = ap.parse_args()

    sfx = "_mots" if args.mots else ""
    fit(args.disc_banner, 96, 32, (0, 0, 0, 255)).convert("RGB").save(os.path.join(HERE, f"banner{sfx}.png"))
    fit(args.card_banner, 96, 32, (0, 0, 0, 0)).save(os.path.join(HERE, f"card_banner{sfx}.png"))
    fit(args.icon, 32, 32, (0, 0, 0, 0)).save(os.path.join(HERE, f"card_icon{sfx}.png"))


if __name__ == "__main__":
    main()

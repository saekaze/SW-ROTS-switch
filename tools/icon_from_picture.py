#!/usr/bin/env python3
"""Make the homebrew menu icon from any picture: cut to a centred square, scaled to 256x256, saved as JPEG.

    python3 tools/icon_from_picture.py my_picture.png switch/icon.jpg

Takes whatever Pillow reads (JPEG, PNG, WebP, BMP, GIF: the first frame). Transparent parts become black.
"""
import sys

from PIL import Image, ImageOps


def main(src, dst):
    im = Image.open(src)
    im.seek(0)
    im = ImageOps.exif_transpose(im)  # phone photos: the way they were taken
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        back = Image.new("RGB", im.size, (0, 0, 0))
        back.paste(im, mask=im.getchannel("A"))
        im = back
    im = ImageOps.fit(im.convert("RGB"), (256, 256), Image.LANCZOS, centering=(0.5, 0.5))
    im.save(dst, "JPEG", quality=92)
    print(f"icon: {src} -> {dst} (256x256)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} picture output.jpg")
    main(sys.argv[1], sys.argv[2])

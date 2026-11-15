#!/usr/bin/env python3
"""The homebrew icon from your own game: the dashboard title image inside default.xbe ($$XTIMAGE, an XPR0 file,
DXT1, data after its header), scaled to the 256x256 JPEG nx_create_nro takes.

    python3 tools/make_icon.py path/to/default.xbe [switch/icon.jpg]     # needs Pillow

The icon is made on your computer and never committed (switch/icon.jpg is in .gitignore). Your own picture in
switch/icon.jpg instead works too: build_from_xbe.sh leaves an existing one alone.
"""
import os
import struct
import sys

from PIL import Image


def title_image(xbe):
    d = open(xbe, "rb").read()
    if d[:4] != b"XBEH":
        sys.exit(f"{xbe}: not an XBE")
    base = struct.unpack_from("<I", d, 0x104)[0]
    nsec, shdr = struct.unpack_from("<II", d, 0x11C)
    for i in range(nsec):
        _, _, _, raw, rsz, name = struct.unpack_from("<6I", d, shdr - base + i * 0x38)
        if d[name - base:name - base + 10] == b"$$XTIMAGE\0":
            xpr = d[raw:raw + rsz]
            if xpr[:4] != b"XPR0":
                sys.exit("the title image is not an XPR0 file")
            hdr = struct.unpack_from("<I", xpr, 8)[0]
            fmt = struct.unpack_from("<I", xpr, 0x18)[0]
            if (fmt >> 8) & 0xFF != 0x0C:
                sys.exit("the title image is not DXT1")
            w, h = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
            return Image.frombytes("RGBA", (w, h), xpr[hdr:hdr + w * h // 2], "bcn", 1)
    sys.exit("no $$XTIMAGE section in the XBE")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                                               "switch", "icon.jpg")
    im = title_image(sys.argv[1]).resize((256, 256), Image.LANCZOS)
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    bg.alpha_composite(im)
    bg.convert("RGB").save(out, quality=92)
    print(f"icon: {out}")


if __name__ == "__main__":
    main()

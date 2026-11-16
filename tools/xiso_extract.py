#!/usr/bin/env python3
"""Extract an original Xbox disc image (XISO / XDVDFS) into a folder.

    python3 tools/xiso_extract.py "Star Wars Episode III.iso" out_folder

Takes both kinds of image: a trimmed XISO (the game partition only, as extract-xiso and most tools write) and a
full disc dump (redump style), where the game partition starts further in. Only reads the image.

The format: sector 32 of the game partition holds the volume descriptor ("MICROSOFT*XBOX*MEDIA", then the root
directory's sector and size). A directory is a binary tree of entries, each: left and right child offsets (in 4 byte
units, 0 = none), first sector, size, attributes (0x10 = directory), name length, name. Entries never cross a
2048 byte sector; the rest of a sector is padded with 0xFF.
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# where the game partition starts: a trimmed XISO, then full dumps of the three disc generations
PARTITIONS = (0, 0x18300000, 0x0FD90000, 0x02080000)


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    return None


def read_dir(f, base, sector, size):
    """The entries of one directory: (name, first sector, size, is_dir)."""
    if size == 0:
        return []
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    entries, stack, seen = [], [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF:  # padding: an empty directory
            continue
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        entries.append((name, start, length, bool(attr & 0x10)))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return entries


def safe(name):
    # names come from the image: no path separators or parent links
    if not name or name in (".", "..") or "/" in name or "\\" in name or "\0" in name:
        raise ValueError(f"bad name in image: {name!r}")
    return name


def extract(image, out, progress=print):
    with open(image, "rb") as f:
        base = find_partition(f)
        if base is None:
            raise SystemExit("not an Xbox disc image (no XDVDFS volume found)")
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root_sector, root_size = struct.unpack("<II", f.read(8))
        total = [0, 0]

        def walk(sector, size, dest):
            os.makedirs(dest, exist_ok=True)
            for name, start, length, is_dir in read_dir(f, base, sector, size):
                path = os.path.join(dest, safe(name))
                if is_dir:
                    walk(start, length, path)
                    continue
                if os.path.isfile(path) and os.path.getsize(path) == length:
                    total[0] += 1  # already there (a run again after an interruption)
                    continue
                f.seek(base + start * SECTOR)
                left = length
                with open(path + ".part", "wb") as o:
                    while left:
                        chunk = f.read(min(left, 8 << 20))
                        if not chunk:
                            raise SystemExit(f"image ends inside {path}")
                        o.write(chunk)
                        left -= len(chunk)
                os.replace(path + ".part", path)
                total[0] += 1
                total[1] += length
                if total[0] % 200 == 0:
                    progress(f"  {total[0]} files, {total[1] >> 20} MB")

        walk(root_sector, root_size, out)
        progress(f"  {total[0]} files, {total[1] >> 20} MB written")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} image.iso out_folder")
    extract(sys.argv[1], sys.argv[2])

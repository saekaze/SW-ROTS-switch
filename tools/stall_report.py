#!/usr/bin/env python3
"""Names the freeze watchdog's thread stacks ("stall:" lines in rots.log) with the build's ELF.

    python3 tools/stall_report.py logs/rots_001.log build-switch/rots.elf

Needs devkitA64's addr2line (DEVKITPRO, default /opt/devkitpro). Each offset is into rots.nro, which is the ELF's
own address.
"""
import os
import re
import subprocess
import sys

log, elf = sys.argv[1], sys.argv[2]
a2l = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64/bin/aarch64-none-elf-addr2line")
lines = [l for l in open(log, encoding="utf-8", errors="replace") if "stall:" in l]
offs = sorted({o for l in lines for o in re.findall(r"\b([0-9A-F]{8})\b", l.split("stall:", 1)[1]) if o != "FFFFFFFF"})
names = {}
if offs:
    out = subprocess.run([a2l, "-f", "-C", "-e", elf] + ["0x" + o for o in offs], capture_output=True, text=True).stdout.split("\n")
    for i, o in enumerate(offs):
        fn, where = out[2 * i], out[2 * i + 1]
        names[o] = f"{fn}  ({os.path.basename(where)})"
for l in lines:
    print(l.rstrip())
    m = re.match(r".*stall: (\S+) pc (\w+) lr (\w+):(.*)", l)
    if not m:
        continue
    for o in [m.group(2), m.group(3)] + m.group(4).split():
        if o in names:
            print(f"      {o}  {names[o]}")

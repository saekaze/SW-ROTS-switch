#!/usr/bin/env bash
# Builds ROTS-Switch-Builder.exe with MinGW-w64 (Linux: apt install mingw-w64). The release workflow runs this.
#   tools/windows/build_exe.sh v0.2.0      # the tag the exe builds
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REF="${1:-main}"
OUT="${2:-$HERE/../../out}"
mkdir -p "$OUT"
# builder.sh as a C string
python3 - "$HERE/builder.sh" "$OUT/builder_script.h" <<'PY'
import sys
text = open(sys.argv[1], encoding="utf-8").read().replace("\r\n", "\n")
def esc(line):
    # octal escapes (at most 3 digits, so they never run into the next character) for anything not plain
    return "".join(chr(b) if 32 <= b <= 126 and b not in (34, 92, 63) else "\\%03o" % b for b in line.encode("utf-8"))
lines = ['"%s\\n"' % esc(l) for l in text.split("\n")[:-1]]
open(sys.argv[2], "w").write("static const char kBuilderScript[] =\n" + "\n".join(lines) + ";\n")
PY
x86_64-w64-mingw32-gcc -O2 -municode -Wall -Wno-unused-function -DROTS_REF="\"$REF\"" -I"$OUT" \
    "$HERE/rots_builder.c" -o "$OUT/ROTS-Switch-Builder.exe" -lshell32 -lole32 -static
x86_64-w64-mingw32-strip "$OUT/ROTS-Switch-Builder.exe"
echo "built $OUT/ROTS-Switch-Builder.exe ($REF)"

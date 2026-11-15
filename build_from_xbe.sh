#!/usr/bin/env bash
# Build rots.nro from YOUR OWN copy of the game, in one go:
#
#   ./build_from_xbe.sh /path/to/extracted/disc            # the folder with default.xbe in it
#   ./build_from_xbe.sh /path/to/extracted/disc --copy-game   # also copy the disc into the SD layout
#
# Result: out/switch_sd/switch/rots/  (rots.nro, rots-settings.toml, game/): copy out/switch_sd/switch to the SD card.
#
# Needs (Linux or WSL2): clang, cmake, ninja, python3 with Pillow, git, and devkitPro with
#   switch-dev deko3d switch-zstd switch-expat switch-zlib   (sudo dkp-pacman -S ...)
# Everything made from the game (game/assets, game/generated, the icon, the NRO) stays on this computer.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DISC="${1:-}"
COPY="${2:-}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
fail() { echo "error: $*" >&2; exit 1; }

[ -n "$DISC" ] || fail "usage: $0 /path/to/extracted/disc [--copy-game]"
[ -f "$DISC/default.xbe" ] || fail "no default.xbe in $DISC (extract your disc with extract-xiso first)"
for t in clang++ cmake ninja python3 git; do command -v "$t" >/dev/null || fail "$t is not installed"; done
python3 -c "import PIL" 2>/dev/null || fail "python3 Pillow is not installed (pip install Pillow)"
[ -f "$DEVKITPRO/cmake/Switch.cmake" ] || fail "devkitPro not found at $DEVKITPRO"
[ -f "$DEVKITPRO/libnx/include/deko3d.h" ] || fail "deko3d missing: sudo dkp-pacman -S deko3d"

# the title: Star Wars Episode III (title ID 4C410017); the PAL release is the one this port was made with
python3 - "$DISC/default.xbe" <<'PY' || fail "this default.xbe is not Revenge of the Sith (Xbox)"
import struct, sys, hashlib
d = open(sys.argv[1], "rb").read()
base, cert = struct.unpack_from("<I", d, 0x104)[0], struct.unpack_from("<I", d, 0x118)[0]
tid = struct.unpack_from("<I", d, cert - base + 8)[0]
if tid != 0x4C410017: sys.exit(1)
sha = hashlib.sha1(d).hexdigest()
if sha != "b6e90f10d11a11207bc512fab5fa1f6b24b7bea0":
    print(f"note: not the tested PAL default.xbe (SHA-1 {sha}); trying anyway")
PY

echo "== 1/5 libraries (third_party)"
bash "$ROOT/tools/fetch_third_party.sh"

echo "== 2/5 the recompiler"
cmake -S "$ROOT/tools/xbe_tools" -B "$ROOT/build-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
ninja -C "$ROOT/build-tools"

echo "== 3/5 translating default.xbe to C++ (game/generated, a few seconds)"
mkdir -p "$ROOT/game/assets"
cp "$DISC/default.xbe" "$ROOT/game/assets/default.xbe"
(cd "$ROOT/game" && "$ROOT/build-tools/xbe_recomp" config/game.toml --xdk "$ROOT/sdk/xdk" | tail -3)

echo "== 4/5 the icon from your default.xbe"
if [ -f "$ROOT/switch/icon.jpg" ]; then
    echo "keeping your switch/icon.jpg"
else
    python3 -I "$ROOT/tools/make_icon.py" "$ROOT/game/assets/default.xbe" "$ROOT/switch/icon.jpg"
fi

echo "== 5/5 the Switch NRO (10-20 minutes the first time)"
mem_mb=$(( $(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 1024 ))
JOBS="${JOBS:-$(( mem_mb / 2500 > 0 ? mem_mb / 2500 : 1 ))}"
[ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
JOBS=$JOBS bash "$ROOT/switch/build.sh"

OUT="$ROOT/out/switch_sd/switch/rots"
mkdir -p "$OUT/game"
cp "$ROOT/build-switch/rots.nro" "$OUT/rots.nro"
[ -f "$OUT/rots-settings.toml" ] || cp "$ROOT/switch/rots-settings.toml" "$OUT/rots-settings.toml"
if [ "$COPY" = "--copy-game" ]; then
    echo "copying the disc into $OUT/game"
    cp -r "$DISC"/. "$OUT/game/"
fi
echo
echo "done: $OUT"
echo "  copy out/switch_sd/switch to the root of the SD card; the game files go in sdmc:/switch/rots/game/"
echo "  (use --copy-game to have them copied there too). Start it from hbmenu while holding R on a game."

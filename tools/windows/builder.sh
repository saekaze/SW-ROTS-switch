#!/usr/bin/env bash
# The part of ROTS-Switch-Builder.exe that runs inside its own WSL distribution ("rots-builder", Ubuntu 24.04,
# root). The exe writes params.txt next to this script, one value a line:
#   1: the disc image (.iso) or the extracted disc folder (a Windows path)
#   2: the output folder (a Windows path): switch/rots/ is made in it
#   3: the git tag of SW-ROTS-switch to build
#   4: a picture for the icon (a Windows path), or empty for the game's own
# Everything happens on this computer: nothing of the game is uploaded anywhere.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mapfile -t P < <(tr -d '\r' < "$HERE/params.txt")
IN_WIN="${P[0]:-}"; OUT_WIN="${P[1]:-}"; REF="${P[2]:-main}"; ICON_WIN="${P[3]:-}"
REPO="https://github.com/saekaze/SW-ROTS-switch"
SRC=/root/SW-ROTS-switch
export DEBIAN_FRONTEND=noninteractive DEVKITPRO=/opt/devkitpro
step() { echo; echo "== $*"; }
fail() { echo; echo "ERROR: $*" >&2; exit 1; }

IN="$(wslpath -u "$IN_WIN")" || fail "cannot read the path $IN_WIN"
OUT="$(wslpath -u "$OUT_WIN")" || fail "cannot use the output folder $OUT_WIN"
[ -e "$IN" ] || fail "not found: $IN_WIN"

step "1/6 build tools (the first time only, a few minutes)"
need=()
for t in clang++ cmake ninja git curl wget python3; do command -v "$t" >/dev/null || need+=("$t"); done
python3 -c "import PIL" 2>/dev/null || need+=(pil)
if [ ${#need[@]} -gt 0 ]; then
    apt-get update -q
    apt-get install -y -q --no-install-recommends clang cmake ninja-build python3 python3-pil git curl wget \
        ca-certificates pkg-config build-essential xz-utils sudo gnupg apt-transport-https
else
    echo "already installed"
fi

step "2/6 devkitPro, the Switch homebrew toolchain (the first time only)"
if [ ! -f "$DEVKITPRO/libnx/include/deko3d.h" ] || [ ! -f "$DEVKITPRO/cmake/Switch.cmake" ]; then
    if ! command -v dkp-pacman >/dev/null; then
        wget -q -O /tmp/install-devkitpro-pacman https://apt.devkitpro.org/install-devkitpro-pacman
        chmod +x /tmp/install-devkitpro-pacman
        yes | /tmp/install-devkitpro-pacman
    fi
    dkp-pacman -Sy --noconfirm --needed switch-dev deko3d switch-zstd switch-expat switch-zlib
else
    echo "already installed"
fi

step "3/6 the port's source code ($REF)"
if [ -d "$SRC/.git" ]; then
    git -C "$SRC" fetch -q --tags --force origin
else
    git clone -q "$REPO" "$SRC"
fi
git -C "$SRC" checkout -q -f "$REF"
echo "SW-ROTS-switch $(git -C "$SRC" describe --tags --always)"

step "4/6 your game files"
GAME="$OUT/switch/rots/game"
mkdir -p "$GAME"
if [ -d "$IN" ]; then
    [ -f "$IN/default.xbe" ] || fail "no default.xbe in $IN_WIN: pick the folder that has default.xbe in it"
    if [ "$(realpath "$IN")" != "$(realpath "$GAME")" ]; then
        echo "copying $IN_WIN to the SD folder (2.4 GB, a few minutes)"
        cp -r -u "$IN"/. "$GAME"/
    fi
else
    echo "extracting the disc image (2.4 GB, a few minutes)"
    python3 -I "$SRC/tools/xiso_extract.py" "$IN" "$GAME"
fi
[ -f "$GAME/default.xbe" ] || fail "the disc has no default.xbe"

step "5/6 the icon"
rm -f "$SRC/switch/icon.jpg"
if [ -n "$ICON_WIN" ]; then
    ICON="$(wslpath -u "$ICON_WIN")"
    python3 -I "$SRC/tools/icon_from_picture.py" "$ICON" "$SRC/switch/icon.jpg" \
        || fail "could not read the picture $ICON_WIN (use a JPG or PNG)"
else
    echo "the game's own picture (from your default.xbe)"
fi

step "6/6 building rots.nro (10 to 20 minutes the first time, less after)"
bash "$SRC/build_from_xbe.sh" "$GAME"
cp "$SRC/build-switch/rots.nro" "$OUT/switch/rots/rots.nro"
[ -f "$OUT/switch/rots/rots-settings.toml" ] || cp "$SRC/switch/rots-settings.toml" "$OUT/switch/rots/rots-settings.toml"
echo
echo "DONE"

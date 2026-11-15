#!/usr/bin/env bash
# The libraries the Switch build compiles (glslang, SPIRV-Cross, uam, imgui, toml++, Zydis, Vulkan headers, SMAA),
# as sheroes-nx vendors them, from its xbe-recomp branch at the commit this port started from.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -d "$ROOT/third_party/glslang" ] && { echo "third_party already there"; exit 0; }
TMP="$(mktemp -d)"
git clone -q --depth 1 --branch xbe-recomp https://github.com/FurryLilManChild/sheroes-nx "$TMP/s"
git -C "$TMP/s" checkout -q 5abfcf14f333c7066d9c8469ffcaecfdccd500ff 2>/dev/null || true
mv "$TMP/s/third_party" "$ROOT/third_party"
rm -rf "$TMP"
echo "third_party ready"

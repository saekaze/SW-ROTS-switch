# Builds switch_sd\switch\sheroes-nx\ beside the repo: sheroes.nro, the default settings, LICENSES.txt and an empty game\
# folder for the player's own files. Nothing of the game goes in: the player copies those.
# package_switch.ps1 [-Build build-switch-pgo]  (run tools/make_licenses.py first when a license changes)
param([string]$Build = 'build-switch')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'switch_sd\switch\sheroes-nx'
$nro = Join-Path $root "$Build\sheroes.nro"
if (-not (Test-Path $nro)) { throw "No ${nro}: build it first (switch/build.sh)" }
# only the package's own files are replaced: anything else there (a tester's logs copied back, say) stays
New-Item -ItemType Directory -Force (Join-Path $out 'game') | Out-Null
Copy-Item $nro $out -Force
Copy-Item (Join-Path $root 'switch\rots-settings.toml') $out -Force
Set-Content (Join-Path $out 'game\PUT-THE-GAME-HERE.txt') -Encoding utf8 -Value @'
Copy the files of your Bloody Roar Extreme disc (Xbox) here: default.xbe with all its folders,
or an .iso image of the disc. They are the same files as the assets folder of the Windows package.
'@

# every license of what the NRO carries (tools/make_licenses.py writes it)
Copy-Item (Join-Path $root 'LICENSES.txt') $out -Force

# the shaders a console made in play (installer/release/shadercache, copied from sdmc:/switch/sheroes/shadercache): the .dk2
# files are the same on every Switch, so a player starts without the first-time stutters. Rebuild it when shaders.cpp
# changes how shaders are made.
$cache = Join-Path $root 'installer\release\shadercache'
if (Test-Path $cache) { Copy-Item $cache $out -Recurse -Force }
Get-ChildItem -Recurse $out | Select-Object FullName, Length

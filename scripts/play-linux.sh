#!/bin/sh
# Starts Leonida's D3D12 runtime (d3d12/, a Windows program) on Linux under Wine, with the VKD3D-Proton that comes in
# the package (scripts/fetch_vkd3d_proton.py puts it in vkd3d-proton/ beside this script), so nothing else needs to be
# downloaded for Direct3D 12. Wine itself and a Vulkan driver (Mesa or NVIDIA's) are the system's.
#
#   ./play-linux.sh /path/to/your/game_scarlett_master.exe [--series-x]
#
# Leonida keeps its own Wine prefix (LEONIDA_WINEPREFIX, default ~/.local/share/leonida/prefix): the bundled
# d3d12.dll and d3d12core.dll are installed there and preferred over Wine's (WINEDLLOVERRIDES), so no other prefix
# is changed. LEONIDA_* variables already set win over the defaults below.
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 /path/to/game_scarlett_master.exe [--series-x]" >&2
    exit 2
fi
game_exe=$1
shift
console=series-s
if [ "${1:-}" = "--series-x" ]; then console=series-x; fi
[ -f "$game_exe" ] || { echo "No $game_exe" >&2; exit 1; }

here=$(cd "$(dirname "$0")" && pwd)
runtime=${LEONIDA_RUNTIME:-$here/leonida.exe}
vkd3d=${LEONIDA_VKD3D:-$here/vkd3d-proton}
[ -f "$runtime" ] || { echo "No $runtime (the Windows build of d3d12/)" >&2; exit 1; }
for dll in d3d12.dll d3d12core.dll; do
    [ -f "$vkd3d/$dll" ] || { echo "No $vkd3d/$dll: run scripts/fetch_vkd3d_proton.py when packaging" >&2; exit 1; }
done
command -v wine >/dev/null 2>&1 || { echo "Wine is needed (your distribution's wine package, or Proton's)" >&2; exit 1; }

data=${XDG_DATA_HOME:-$HOME/.local/share}/leonida
export WINEPREFIX=${LEONIDA_WINEPREFIX:-$data/prefix}
export WINEARCH=win64
if [ ! -d "$WINEPREFIX/drive_c/windows/system32" ]; then
    echo "Leonida: making its Wine prefix in $WINEPREFIX"
    mkdir -p "$WINEPREFIX"
    WINEDEBUG=-all wineboot -u >/dev/null 2>&1
fi
# The bundled VKD3D-Proton, copied in when it is missing or differs (a new package).
system32=$WINEPREFIX/drive_c/windows/system32
for dll in d3d12.dll d3d12core.dll; do
    if ! cmp -s "$vkd3d/$dll" "$system32/$dll"; then cp "$vkd3d/$dll" "$system32/$dll"; fi
done
export WINEDLLOVERRIDES="d3d12,d3d12core=n${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"

# Where the title's files are, and where the caches and saves go.
export LEONIDA_GAME_DIR=${LEONIDA_GAME_DIR:-$(cd "$(dirname "$game_exe")" && pwd)}
export LEONIDA_CACHE_DIR=${LEONIDA_CACHE_DIR:-$data/cache}
export LEONIDA_DATA_DIR=${LEONIDA_DATA_DIR:-$data/data}
mkdir -p "$LEONIDA_CACHE_DIR" "$LEONIDA_DATA_DIR"

# The settings scripts/play-nvidia.ps1 measured (d3d12/README.md), as defaults.
set_default() { eval "[ -n \"\${$1:-}\" ] || export $1=\"$2\""; }
set_default LEONIDA_TEXTURE_NO_WAIT 1
set_default LEONIDA_RELAY 2
set_default LEONIDA_CARD_STEPS 2
set_default LEONIDA_DYNAMIC_SLOTS 524288
set_default LEONIDA_PIPELINE_WAIT_MS 10000
set_default LEONIDA_RENDER_SCALE 0.5
set_default LEONIDA_OWN_UPSCALER 1
set_default LEONIDA_BVH_MISS 1
set_default LEONIDA_CARD_BUFFERS_MIN_MB 1
set_default LEONIDA_CARD_BUFFERS_MB 1536
set_default LEONIDA_COMPOSE 1
set_default LEONIDA_NAN_SCRUB 1
set_default LEONIDA_AMBIENT_AIR 1.5
set_default LEONIDA_AMBIENT_MOST 2

echo "Leonida: $console under Wine with the bundled VKD3D-Proton ($(head -n 1 "$vkd3d/SOURCE.txt" 2>/dev/null || echo bundled))"
exec wine "$runtime" run "$game_exe" --execute --console "$console"

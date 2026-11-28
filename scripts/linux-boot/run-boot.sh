#!/bin/bash
# Boot gta6rt as its own systemd --user unit, protected from the swarm and logged in full.
#
# usage: run-boot.sh <agent> <n> <gta6rt.exe> [seconds=150] [KEY=VALUE ...]
#   e.g. run-boot.sh fsdraw 3 /path/wt-fsdraw/runtime/build/boot/gta6rt.exe 150 LEONIDA_FOO=1
#
# What it does for you:
#  - runs the boot outside your agent's process tree (survives agent/session restarts);
#  - waits for memory headroom before starting, then holds the GPU lock AND the build lock for the whole boot,
#    so no compile competes with the game for RAM;
#  - Series S profile, late-frame capture, source capture and Wine's error channel on by default (override with KEY=VALUE);
#  - samples RAM / swap / the game's memory / VRAM every 2 s;
#  - after the boot, even when systemd-oomd or a crash kills it, writes a digest with the unit result, OOM and GPU
#    kernel events and every distinct error line, and appends one line to the shared ledger /opt/leonida/work/boots.tsv.
#
# Outputs (in /opt/leonida/work/<agent>/): boot-<n>.log  boot-<n>.resources.tsv  boot-<n>.digest.txt
set -u
AGENT=${1:?agent}; N=${2:?n}; EXE=${3:?gta6rt.exe}; SECS=${4:-150}; shift $(( $# >= 4 ? 4 : $# ))
if [ "$AGENT" = "." ] || [ "$AGENT" = ".." ] || ! [[ "$AGENT" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "run-boot: invalid agent: $AGENT" >&2; exit 2
fi
if ! [[ "$N" =~ ^[0-9]+$ ]]; then
  echo "run-boot: invalid boot number: $N" >&2; exit 2
fi
if ! [[ "$SECS" =~ ^[1-9][0-9]*$ ]]; then
  echo "run-boot: invalid seconds (expected positive integer): $SECS" >&2; exit 2
fi
for kv in "$@"; do
  if ! [[ "$kv" =~ ^[A-Za-z_][A-Za-z0-9_]*= ]]; then
    echo "run-boot: invalid environment override (expected KEY=VALUE): $kv" >&2; exit 2
  fi
done
[ -f "$EXE" ] || { echo "run-boot: no such gta6rt: $EXE" >&2; exit 2; }
# gta6rt loads gta6rt_tls_host.dll from its own folder; without it the title gets no module TLS index and exits at once.
[ -f "$(dirname "$EXE")/gta6rt_tls_host.dll" ] || { echo "run-boot: $(dirname "$EXE")/gta6rt_tls_host.dll is missing: copy it next to the exe (build it with the gta6rt target) or boot the exe from its build folder" >&2; exit 2; }
ROOT=${LEONIDA_BOOT_ROOT:-/opt/leonida}
WIN_ROOT=${ROOT#/}; WIN_ROOT="Z:\\${WIN_ROOT//\//\\}"
WORK=$ROOT/work/$AGENT; mkdir -p "$WORK"
UNIT=leonida-boot-$AGENT-$N
if systemctl --user is-active --quiet "$UNIT"; then echo "run-boot: $UNIT is already running" >&2; exit 2; fi

declare -A ENV=(
  [WINE]="$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine"
  [WINEPREFIX]="$HOME/.local/share/leonida-proton/pfx"
  [WINEDEBUG]="-all,err+all"
  [GTA6RT]="$EXE"
  [LEONIDA_CAPTURE]="$WIN_ROOT\\capture\\$AGENT-$N"
  [LEONIDA_CAPTURE_EVERY]=4800 [LEONIDA_CAPTURE_LIMIT]=24 [LEONIDA_CAPTURE_SOURCE]=1
  [LEONIDA_DEVICE_TYPE]=6
  [LEONIDA_PIPELINE_CACHE_DIR]="$WIN_ROOT\\cache\\pipelines"
)
for kv in "$@"; do ENV[${kv%%=*}]="${kv#*=}"; done
args=()
for k in "${!ENV[@]}"; do args+=(--setenv="$k=${ENV[$k]}"); done

echo "run-boot: $UNIT starting (queues for memory headroom, the GPU lock and the build lock)"
rm -f "$WORK/boot-$N.digest.txt"
systemd-run --user --collect --quiet --unit="$UNIT" -p WorkingDirectory="$ROOT" \
  --setenv=BOOT_AGENT="$AGENT" --setenv=BOOT_N="$N" --setenv=BOOT_SECS="$SECS" "${args[@]}" \
  "$ROOT/boot-inner.sh" || { echo "run-boot: could not start $UNIT" >&2; exit 2; }
# The digest is written by a separate, tiny watcher unit: it survives systemd-oomd killing the boot's cgroup and the
# caller's shell going away. It waits for the boot unit to end, reads its result from the journal and writes the digest.
systemd-run --user --collect --quiet --unit="leonida-digest-$AGENT-$N" -p MemoryMax=256M \
  /bin/bash -c 'while systemctl --user is-active --quiet "$0" || systemctl --user is-activating --quiet "$0"; do sleep 3; done
    START=$(sed -n "s/^start_epoch=//p" "$3/work/$1/boot-$2.meta" 2>/dev/null)
    RES=$(journalctl --user -u "$0" ${START:+--since "@$START"} -q --no-pager 2>/dev/null | grep -o "Failed with result .[a-z-]*." | tail -1 | tr -d "\x27" | sed "s/Failed with result //")
    SERVICE_RESULT=${RES:-success} exec "$3/boot-digest.sh" "$1" "$2"' "$UNIT" "$AGENT" "$N" "$ROOT"
# Block until the digest exists (so a caller that waits on this script gets the result), then print it.
while [ ! -s "$WORK/boot-$N.digest.txt" ] || systemctl --user is-active --quiet "leonida-digest-$AGENT-$N"; do sleep 5; done
echo "run-boot: $UNIT finished. Digest:"
cat "$WORK/boot-$N.digest.txt"
head -1 "$WORK/boot-$N.digest.txt" | grep -q ': OK' && exit 0 || exit 1

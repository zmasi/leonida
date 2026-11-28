#!/bin/bash
# Runs inside the leonida-boot-* unit (see run-boot.sh). Waits for memory, takes the locks, samples resources, boots.
set -u

die() {
  echo "ERROR: $*" >&2
  exit 2
}

OUT=${LEONIDA_BOOT_ROOT:-${LEONIDA_OUT:-/opt/leonida}}
BOOT_AGENT=${BOOT_AGENT:-}
BOOT_N=${BOOT_N:-}
BOOT_SECS=${BOOT_SECS:-}
GTA6RT=${GTA6RT:-}
BOOT_SH=${BOOT_SH:-$OUT/boot.sh}
NVIDIA_SMI=${NVIDIA_SMI:-nvidia-smi}
PROC_MEMINFO=${LEONIDA_MEMINFO_PATH:-/proc/meminfo}
CGROUP_SELF=${LEONIDA_SELF_CGROUP_PATH:-/proc/self/cgroup}
CGROUP_ROOT=${LEONIDA_CGROUP_ROOT:-/sys/fs/cgroup}
WAIT_SLEEP=${LEONIDA_WAIT_SLEEP_SECS:-15}
SAMPLE_SLEEP=${LEONIDA_SAMPLE_SLEEP_SECS:-2}
WAIT_ITERS=${LEONIDA_WAIT_ITERS:-40}

[ -n "$BOOT_AGENT" ] || die "BOOT_AGENT must be set"
[ -n "$BOOT_N" ] || die "BOOT_N must be set"
[ -n "$BOOT_SECS" ] || die "BOOT_SECS must be set"
[ -n "$GTA6RT" ] || die "GTA6RT must be set"
case "$BOOT_AGENT" in
  .|..|*/*|*\\*|*..*) die "BOOT_AGENT must be a single path segment: $BOOT_AGENT" ;;
esac
case "$BOOT_N" in
  .|..|*/*|*\\*|*..*) die "BOOT_N must be a single path segment: $BOOT_N" ;;
esac
[[ "$BOOT_SECS" =~ ^[0-9]+$ ]] || die "BOOT_SECS must be a non-negative integer: $BOOT_SECS"
[[ "$WAIT_ITERS" =~ ^[0-9]+$ ]] || die "WAIT_ITERS must be a non-negative integer: $WAIT_ITERS"

NEED=${LEONIDA_BOOT_MIN_FREE_MB:-6500}
[[ "$NEED" =~ ^[0-9]+$ ]] || die "LEONIDA_BOOT_MIN_FREE_MB must be a non-negative integer: $NEED"

WORK=${LEONIDA_BOOT_WORK:-$OUT/work/$BOOT_AGENT}; N=$BOOT_N; LOG=$WORK/boot-$N.log; RES=$WORK/boot-$N.resources.tsv
META=$WORK/boot-$N.meta
mkdir -p "$OUT" "$WORK" || die "cannot create work directory: $WORK"
: > "$LOG"
echo "start_epoch=$(date +%s)" > "$META"
echo "unit=leonida-boot-$BOOT_AGENT-$N gta6rt=$GTA6RT secs=$BOOT_SECS" >> "$META"
env | grep -E '^(LEONIDA_|WINEDEBUG=)' | sort >> "$META" || true

mem_free_mb() {  # RAM available plus a third of free zram swap (zram pages cost RAM, roughly 3:1)
  [ -r "$PROC_MEMINFO" ] || { echo 0; return; }
  awk 'BEGIN{a=0; s=0} /^MemAvailable:/{a=$2} /^SwapFree:/{s=$2} END{printf "%d", (a + s/3)/1024}' "$PROC_MEMINFO"
}
swap_pct() {
  [ -r "$PROC_MEMINFO" ] || { echo 0; return; }
  awk 'BEGIN{t=0; f=0} /^SwapTotal:/{t=$2} /^SwapFree:/{f=$2} END{pct=(t>0 ? (t-f)*100/t : 0); printf "%d", pct}' "$PROC_MEMINFO"
}
read_uint() {
  local v=""
  if [ -r "$1" ]; then
    v=$(cat "$1" 2>/dev/null || true)
    v=${v//$'\r'/}
  fi
  [[ "$v" =~ ^[0-9]+$ ]] || v=0
  printf '%s' "$v"
}

SAMPLER=""
cleanup() {
  if [ -n "${SAMPLER:-}" ]; then
    kill "$SAMPLER" 2>/dev/null || true
    wait "$SAMPLER" 2>/dev/null || true
    SAMPLER=""
  fi
}
trap cleanup EXIT INT TERM

exec 8>"$OUT/.gpu.lock"
if command -v flock >/dev/null 2>&1; then flock 8; fi   # one boot at a time
echo "[run-boot] $(date +%T) got the GPU lock" >> "$LOG"
for ((i = 1; i <= WAIT_ITERS; i++)); do                 # up to 10 min for memory headroom
  f=$(mem_free_mb); sp=$(swap_pct); [ "$f" -ge "$NEED" ] && [ "$sp" -lt 70 ] && break
  echo "[run-boot] $(date +%T) waiting for memory: ${f} MB free (need ${NEED}), swap ${sp}% (need < 70%)" >> "$LOG"; sleep "$WAIT_SLEEP"
done
echo "[run-boot] $(date +%T) memory before boot: $(mem_free_mb) MB free, swap $(swap_pct)%" >> "$LOG"
exec 9>"$OUT/.build.lock"
if command -v flock >/dev/null 2>&1; then flock 9; fi   # no compiles while the game runs
echo "[run-boot] $(date +%T) got the build lock; booting" >> "$LOG"
echo "boot_epoch=$(date +%s)" >> "$META"

cg_rel=""
if [ -r "$CGROUP_SELF" ]; then
  cg_rel=$(awk -F: '$1=="0"{print $3; exit}' "$CGROUP_SELF" 2>/dev/null || true)
  [ -n "$cg_rel" ] || cg_rel=$(awk -F: 'NF>=3{p=$3} END{print p}' "$CGROUP_SELF" 2>/dev/null || true)
  cg_rel=${cg_rel//$'\r'/}
fi
CG=$CGROUP_ROOT$cg_rel
( trap 'kill "${sleep_pid:-}" 2>/dev/null || true; exit 0' TERM INT
  sleep_pid=""
  printf 'time\tmem_avail_mb\tswap_used_mb\tunit_mem_mb\tunit_swap_mb\tgpu_mem_mb\tgpu_util\n'
  while :; do
    ma=0; su=0
    if [ -r "$PROC_MEMINFO" ]; then
      read -r ma su < <(awk 'BEGIN{a=0; t=0; f=0} /^MemAvailable:/{a=$2} /^SwapTotal:/{t=$2} /^SwapFree:/{f=$2} END{u=(t>f ? t-f : 0); print int(a/1024), int(u/1024)}' "$PROC_MEMINFO")
    fi
    um=$(( $(read_uint "$CG/memory.current") / 1048576 ))
    us=$(( $(read_uint "$CG/memory.swap.current") / 1048576 ))
    g=$("$NVIDIA_SMI" --query-gpu=memory.used,utilization.gpu --format=csv,noheader,nounits 2>/dev/null | head -n 1 | tr -d ' \r' | tr ',' '\t')
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%T)" "$ma" "$su" "$um" "$us" "${g:-?	?}"
    sleep "$SAMPLE_SLEEP" &
    sleep_pid=$!
    wait "$sleep_pid" 2>/dev/null || break
    sleep_pid=""
  done ) > "$RES" 2>/dev/null &
SAMPLER=$!

if [ -f "$BOOT_SH" ]; then
  "$BOOT_SH" run "$BOOT_SECS" >> "$LOG" 2>&1
  rc=$?
else
  echo "[run-boot] missing boot script: $BOOT_SH" >> "$LOG"
  rc=127
fi
cleanup
echo "[run-boot] $(date +%T) boot.sh exit $rc" >> "$LOG"
# boot.sh stops the title with timeout(1) at the time limit: 124 means it ran the whole time, which is success here.
[ "$rc" = 124 ] && { echo "[run-boot] time limit reached" >> "$LOG"; rc=0; }
exit $rc

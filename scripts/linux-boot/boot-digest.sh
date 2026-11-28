#!/bin/bash
# ExecStopPost of a leonida-boot-* unit: runs after the boot ends however it ended (exit, crash, oomd kill, timeout).
# Writes /opt/leonida/work/<agent>/boot-<n>.digest.txt and appends a line to /opt/leonida/work/boots.tsv.
# Local only: digests quote trace lines, so they never go into commits or PRs.
set -u
AGENT=${1:?agent}; N=${2:?n}
if [ "$AGENT" = "." ] || [ "$AGENT" = ".." ] || ! [[ "$AGENT" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "boot-digest: invalid agent: $AGENT" >&2; exit 2
fi
if ! [[ "$N" =~ ^[0-9]+$ ]]; then
  echo "boot-digest: invalid boot number: $N" >&2; exit 2
fi
ROOT=${LEONIDA_BOOT_ROOT:-/opt/leonida}
WORK=$ROOT/work/$AGENT; LOG=$WORK/boot-$N.log; RES=$WORK/boot-$N.resources.tsv
META=$WORK/boot-$N.meta; OUT=$WORK/boot-$N.digest.txt; LEDGER=$ROOT/work/boots.tsv
mkdir -p "$WORK" "$(dirname "$LEDGER")"
START=$(sed -n 's/^start_epoch=//p' "$META" 2>/dev/null); BOOT=$(sed -n 's/^boot_epoch=//p' "$META" 2>/dev/null)
START=${START:-$(date +%s)}; BOOT=${BOOT:-$START}; END=$(date +%s)
RESULT=${SERVICE_RESULT:-?}; ECODE=${EXIT_CODE:-?}; ESTAT=${EXIT_STATUS:-?}
D3D=$(grep -o 'd3d12=[^ ]*' "$LOG" 2>/dev/null | tail -1 | cut -d= -f2)
STDOUT=${D3D/d3d12-run-/stdout-run-}
CAP=$ROOT/capture/$AGENT-$N

verdict=OK
# systemd-oomd can kill the unit's processes without the unit ending in "oom-kill" (boot.sh then exits on its own).
if journalctl --user -u "leonida-boot-$AGENT-$N" --since "@$START" -q --no-pager 2>/dev/null | grep -q 'systemd-oomd killed'; then RESULT=oom-kill; fi
case "$RESULT" in
  oom-kill) verdict="KILLED BY OOM (systemd-oomd or kernel): memory, not a crash" ;;
  timeout) verdict="TIMEOUT" ;;
  exit-code) verdict="EXITED WITH ERROR (code $ECODE status $ESTAT)" ;;
  signal|core-dump) verdict="KILLED BY SIGNAL ($ESTAT)" ;;
  success) verdict="OK (ran to its time limit or exited cleanly)" ;;
esac
grep -q 'time limit reached' "$LOG" 2>/dev/null && [ "$RESULT" = success ] && verdict="OK (ran its full time limit)"

cnt() { local c=0; [ -f "$D3D" ] && c=$(grep -a -c -E "$@" "$D3D"); echo "${c:-0}"; }
{
  echo "== leonida boot $AGENT-$N: $verdict"
  echo "unit result=$RESULT exit_code=$ECODE exit_status=$ESTAT"
  echo "queued $((BOOT-START)) s, ran $((END-BOOT)) s; trace $D3D"
  sed -n '/^LEONIDA_\|^WINEDEBUG\|^unit=/p' "$META" 2>/dev/null | tr '\n' ' '; echo
  echo
  echo "-- counts"
  echo "PresentX ok $(cnt 'XboxPresentation::PresentX ok')  refused $(cnt 'XboxPresentation::PresentX failed')"
  echo "CreatePipelineState failed $(cnt 'CreatePipelineState failed')  FSPX compute misses $(cnt 'no translated compute shader for FSPX')  slot-67 failed $(cnt 'slot 67 call .* failed')"
  echo "E_NOTIMPL $(cnt 'E_NOTIMPL')  wine err: lines $(cnt '^[0-9a-f]{4}:err:|:err:')  exceptions $(cnt -i 'exception|unhandled|page fault|access violation')"
  grep -a -m1 -E 'translated shaders \(' "$D3D" 2>/dev/null
  grep -a 'present-probe: source frame .* via' "$D3D" 2>/dev/null | tail -1 | sed 's/.*via/source via/' | cut -c1-120
  echo
  echo "-- frames (swapchain; then the title's own backbuffer when LEONIDA_CAPTURE_SOURCE=1)"
  [ -s "$CAP/index.txt" ] && { echo "$(wc -l < "$CAP/index.txt") captured; last: $(tail -1 "$CAP/index.txt" | cut -c1-110)"; awk '{m=$(NF-2)+$(NF-1)+$NF; if (m>0) n++} END{print n+0 " of " NR " frames non-black"}' "$CAP/index.txt"; } || echo "no capture index"
  grep -a 'present-probe: source frame .* mean' "$D3D" 2>/dev/null | tail -2 | sed 's/.*source frame/source frame/' | cut -c1-140
  echo
  echo "-- resources (2 s samples)"
  [ -s "$RES" ] && awk -F'\t' 'NR>1{ c++; if(min==""||$2<min)min=$2; if($3>sw)sw=$3; if($4>um)um=$4; if($5>us)us=$5; if($6+0>gm)gm=$6 } END{ if(c>0) printf "min RAM available %s MB, max swap used %s MB, game peak %s MB RAM + %s MB swap, VRAM peak %s MB\n",min,sw,um,us,gm; else print "no samples" }' "$RES" || echo "no samples"
  journalctl --user -u "leonida-boot-$AGENT-$N" --since "@$START" -q --no-pager 2>/dev/null | grep -E 'Consumed|oom|Failed' | sed 's/^.*\]: //' | sort -u | cut -c1-200
  # Compare with other boots instead of guessing: on the console (Series S) path the title itself grows to ~20 GB of
  # RAM+swap by ~130 s (fsdraw-6: 9.9 GB + 11.2 GB), mostly zero pages that zram compresses well.
  [ -s "$RES" ] && awk -F'\t' 'NR>1{ c++; t=$4+$5; if(t>m)m=t } END{ if(c>0) printf "game peak RAM+swap %.1f GB (console-path baseline ~20 GB at 130 s; oomd kills when zram swap passes 98%%)\n", m/1024 }' "$RES"
  echo
  echo "-- system events during the boot (OOM, GPU Xid, segfaults)"
  { journalctl --since "@$BOOT" -q --no-pager 2>/dev/null | grep -i -E 'systemd-oomd.*(Marked|killed)|oom-kill|out of memory|NVRM: Xid|segfault|gta6rt|wineserver.*crash' ; } | cut -c1-200 | tail -15
  echo
  echo "-- distinct error lines (normalised, with counts; top 60)"
  for f in "$D3D" "$STDOUT"; do [ -f "$f" ] && cat "$f"; done 2>/dev/null \
    | grep -a -i -E 'error|fail|E_NOTIMPL|E_INVALIDARG|E_FAIL|0x8[0-9a-f]{7}|exception|assert|abort|unhandled|page fault|refused|not implemented|no translated|no emulation|crash|:err:|fatal|cannot|could not|unsupported|invalid' \
    | grep -a -v -E ' ok$| called$' \
    | sed -E 's/#[0-9]+/#N/g; s/0x[0-9a-fA-F]{6,}/0xADDR/g; s/\b[0-9a-f]{64}\b/SHA/g; s/[0-9]{4,}/NUM/g; s/^[0-9a-f]{4}:/TID:/' \
    | cut -c1-220 | sort | uniq -c | sort -rn | head -60
  echo
  echo "-- where the trace ended"
  [ -f "$D3D" ] && tail -n 8 "$D3D" | cut -c1-200
} > "$OUT" 2>&1

frames=$( [ -s "$CAP/index.txt" ] && awk '{m=$(NF-2)+$(NF-1)+$NF; if (m>0) n++} END{print (n+0) "/" NR}' "$CAP/index.txt" || echo "-")
peak=$( [ -s "$RES" ] && awk -F'\t' 'NR>1{c++; if($4>u)u=$4} END{if(c>0) print u+0; else print "-"}' "$RES" || echo "-")
exec 7>>"$LEDGER"; if command -v flock >/dev/null 2>&1; then flock 7; fi
# Both the unit's ExecStopPost and the outer wrapper call this; the first one to finish records the boot.
awk -F'\t' -v a="$AGENT" -v n="$N" 'NR>1 && $2==a && $3==n {found=1; exit} END{exit !found}' "$LEDGER" 2>/dev/null && exit 0
[ -s "$LEDGER" ] || printf 'end\tagent\tn\tresult\tran_s\tpresentx_ok\tpso_failed\tfspx_miss\tnonblack_frames\tpeak_mb\tgta6rt\n' >&7
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date '+%F %T')" "$AGENT" "$N" "$RESULT" "$((END-BOOT))" \
  "$(cnt 'XboxPresentation::PresentX ok')" "$(cnt 'CreatePipelineState failed')" "$(cnt 'no translated compute shader for FSPX')" \
  "$frames" "$peak" "$(sed -n 's/^unit=.*gta6rt=\([^ ]*\).*/\1/p' "$META" 2>/dev/null)" >&7
exit 0

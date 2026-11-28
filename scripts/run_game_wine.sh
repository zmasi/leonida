#!/usr/bin/env bash
# Run the retail game once under Wine on Linux: `gta6rt.exe --run <exe>` with the startup and D3D12 traces on, sound
# off, a pinned Vulkan device, a hard time limit and full teardown, then print counts only
# (tools/game_run_summary.py). Headless on a private kwin_wayland session by default, or on the current display with
# --visible. `scripts/run_game_wine.sh --help` lists the inputs; docs/linux_proton.md has the requirements.
#
# The run folder's logs can quote what the title prints: they stay on this machine. Share the printed summary.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SELF=$ROOT/scripts/run_game_wine.sh

usage() {
  cat <<'EOF'
Usage: scripts/run_game_wine.sh --build DIR --exe FILE --game-dir DIR --out DIR [options] [-- gta6rt arguments]

Runs `gta6rt.exe --run <exe>` once under Wine, headless or on this display, with sound off, a pinned Vulkan device,
a time limit and the startup and D3D12 traces on, then prints counts only. Every path comes from here or from the
environment; nothing is assumed.

Inputs:
  --build DIR         a build folder holding gta6rt.exe and gta6rt_tls_host.dll, or a checkout: its runtime/ is
                      cross-built with clang-cl and xwin (XWIN_DIR must be set) into --build-dir first
  --build-dir DIR     where a checkout is built (default <checkout>/runtime/build/wine-game-run)
  --exe FILE          the game executable                       (or LEONIDA_GAME_EXE)
  --game-dir DIR      the package folder; the working folder     (or LEONIDA_GAME_DIR)
  --shaders DIR       the translated shader library             (or LEONIDA_SHADERS; "none" leaves it unset)
  --out DIR           the run folder; it must not hold a run yet (logs, traces, saves, overlay, summary)
  --prefix DIR        the Wine prefix                           (or WINEPREFIX; default <out>/prefix)

Options:
  --seconds N         hard time limit of the run (default 600; SIGTERM, then SIGKILL 10 s later)
  --visible           open the window on the current DISPLAY instead of a private headless session
  --device VID:DID    the Vulkan device, as MESA_VK_DEVICE_SELECT=VID:DID! (e.g. 1002:744c); without it Mesa picks
  --screenshot-every S  capture the game window into <out>/shots every S seconds (needs ImageMagick's import and
                      xprop); the shots are game content and stay local
  --lock FILE         the lock that allows one run at a time (default $XDG_RUNTIME_DIR/leonida-run-game-wine.lock,
                      or <out>/run.lock without XDG_RUNTIME_DIR)
  -h, --help          this text
  -- ARGS             further gta6rt arguments, after the fixed ones

Environment:
  WINE                the wine to use (default wine on PATH; for Proton, its files/bin/wine)
  LEONIDA_*           passed to the runtime unchanged (LEONIDA_INPUT_SCRIPT, LEONIDA_ARENA_MB, ...); the launcher
                      sets LEONIDA_AUDIO=silent, and LEONIDA_D3D12=vulkan, LEONIDA_RUMBLE=0 and LEONIDA_SAVE_DIR
                      unless they are set

Headless needs kwin_wayland (with Xwayland) and dbus-run-session: Xvfb has no DRI3, so RADV cannot present there.
Sound: Wine's audio drivers are disabled, PulseAudio, PipeWire and ALSA point at nothing, and the runtime serves its
silent output endpoint (LEONIDA_AUDIO=silent).
EOF
}

die() { echo "run_game_wine.sh: $*" >&2; exit 2; }

if [ "${1:-}" = --inner ]; then
  # ---- inside the session: DISPLAY is kwin's Xwayland (headless) or the caller's display (--visible) ----
  [ "$#" -ge 5 ] || die "--inner requires RUN BUILD LIMIT SHOT_EVERY"
  RUN=$2; BUILD=$3; LIMIT=$4; SHOT_EVERY=$5
  shift 5
  WINE=${WINE:-wine}
  wine_bin=$(command -v "$WINE" 2>/dev/null || true)
  WINESERVER=${wine_bin:+$(dirname "$wine_bin")/wineserver}
  [ -n "$WINESERVER" ] && [ -x "$WINESERVER" ] || WINESERVER=wineserver
  unset WAYLAND_DISPLAY  # Wine's X11 driver on Xwayland, never its Wayland driver
  {
    echo "display: ${DISPLAY:-none}"
    if command -v xdpyinfo >/dev/null 2>&1; then
      echo "DRI3 extension: $(xdpyinfo 2>/dev/null | grep -c -w DRI3)"
    fi
  } >> "$RUN/session.txt"
  mkdir -p "$WINEPREFIX"
  "$WINESERVER" -p
  # Warm-up: starts the prefix's services; no Mono/Gecko prompts in a new prefix.
  WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}mscoree,mshtml=" "$WINE" wineboot -u </dev/null >/dev/null 2>&1
  SHOTS=
  if [ "$SHOT_EVERY" != 0 ]; then
    mkdir -p "$RUN/shots"
    (
      n=0
      while sleep "$SHOT_EVERY"; do
        n=$((n + 1))
        # Wine names the window's class after the executable: gta6rt.exe.
        for id in $(xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -o '0x[0-9a-f]*'); do
          if xprop -id "$id" WM_CLASS 2>/dev/null | grep -qi 'gta6rt'; then
            import -window "$id" "$RUN/shots/$(printf '%04d' "$n").png" 2>/dev/null
            break
          fi
        done
      done
    ) &
    SHOTS=$!
  fi
  cd "$GAME_DIR" || { echo 2 > "$RUN/exit_code"; exit 0; }
  t0=$(date +%s)
  timeout -k 10 "$LIMIT" "$WINE" "$BUILD/gta6rt.exe" --run "Z:$GAME_EXE" --game-dir "Z:$GAME_DIR" \
      --overlay "Z:$RUN/overlay" --trace "Z:$RUN/boot.jsonl" --trace-d3d12 "$@" \
      </dev/null > "$RUN/stdout.log" 2> "$RUN/stderr.log"
  code=$?
  echo "$(( $(date +%s) - t0 ))" > "$RUN/seconds"
  echo "$code" > "$RUN/exit_code"
  if [ -n "$SHOTS" ]; then
    kill "$SHOTS" 2>/dev/null || true
    wait "$SHOTS" 2>/dev/null || true
  fi
  "$WINESERVER" -k 2>/dev/null
  exit 0
fi

# ---- arguments ----
SRC= BUILD_DIR= OUT= PREFIX=${WINEPREFIX:-} LIMIT=600 VISIBLE=0 DEVICE= SHOT_EVERY=0 LOCK=
EXE=${LEONIDA_GAME_EXE:-} GAME=${LEONIDA_GAME_DIR:-} SHADERS=${LEONIDA_SHADERS:-}
need() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 needs a value (see --help)"; }
while [ $# -gt 0 ]; do
  case $1 in
    -h|--help) usage; exit 0 ;;
    --build) need "$@"; SRC=$2; shift 2 ;;
    --build-dir) need "$@"; BUILD_DIR=$2; shift 2 ;;
    --exe) need "$@"; EXE=$2; shift 2 ;;
    --game-dir) need "$@"; GAME=$2; shift 2 ;;
    --shaders) need "$@"; SHADERS=$2; shift 2 ;;
    --out) need "$@"; OUT=$2; shift 2 ;;
    --prefix) need "$@"; PREFIX=$2; shift 2 ;;
    --seconds) need "$@"; LIMIT=$2; shift 2 ;;
    --visible) VISIBLE=1; shift ;;
    --device) need "$@"; DEVICE=$2; shift 2 ;;
    --screenshot-every) need "$@"; SHOT_EVERY=$2; shift 2 ;;
    --lock) need "$@"; LOCK=$2; shift 2 ;;
    --) shift; break ;;
    *) die "unknown argument: $1 (see --help)" ;;
  esac
done

[ -n "$SRC" ] || die "--build is required: a build folder with gta6rt.exe, or a checkout"
[ -n "$EXE" ] || die "--exe (or LEONIDA_GAME_EXE) is required: the game executable"
[ -n "$GAME" ] || die "--game-dir (or LEONIDA_GAME_DIR) is required: the package folder"
[ -n "$OUT" ] || die "--out is required: the run folder"
[ -n "$SHADERS" ] || die "--shaders (or LEONIDA_SHADERS) is required: the translated shader library, or none"
case $LIMIT in ''|*[!0-9]*|0*) die "--seconds must be a whole number of seconds above 0, not '$LIMIT'" ;; esac
case $SHOT_EVERY in ''|*[!0-9]*|0?*) die "--screenshot-every must be a whole number of seconds, not '$SHOT_EVERY'" ;; esac
if [ -n "$DEVICE" ] && ! [[ $DEVICE =~ ^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$ ]]; then
  die "--device takes VID:DID in hex, as lspci -nn prints it (e.g. 1002:744c), not '$DEVICE'"
fi
[ -e "$SRC" ] || die "no build path at $SRC"
[ -f "$EXE" ] || die "no game executable at $EXE"
[ -d "$GAME" ] || die "no package folder at $GAME"
if [ "$SHADERS" != none ]; then
  SHADERS=${SHADERS#Z:}
  [ -d "$SHADERS" ] || die "no shader library at $SHADERS"
fi
EXE=$(readlink -f "$EXE"); GAME=$(readlink -f "$GAME"); SRC=$(readlink -f "$SRC")

# ---- what the host needs ----
WINE=${WINE:-wine}
missing=()
command -v "$WINE" >/dev/null 2>&1 || missing+=("$WINE (Wine)")
for t in timeout flock setsid python3; do command -v $t >/dev/null 2>&1 || missing+=("$t"); done
if [ "$VISIBLE" = 1 ]; then
  [ -n "${DISPLAY:-}" ] || die "--visible needs DISPLAY: run it from the desktop session (an X11 or Xwayland display)"
else
  for t in kwin_wayland dbus-run-session Xwayland; do command -v $t >/dev/null 2>&1 || missing+=("$t"); done
fi
if [ "$SHOT_EVERY" != 0 ]; then
  for t in import xprop; do command -v $t >/dev/null 2>&1 || missing+=("$t (for --screenshot-every)"); done
fi
if [ ${#missing[@]} -gt 0 ]; then
  echo "run_game_wine.sh: missing: ${missing[*]}" >&2
  [ "$VISIBLE" = 1 ] || echo "  headless runs need kwin_wayland with Xwayland: Xvfb has no DRI3, so RADV cannot" \
    "present there. Install KWin, or run on your display with --visible." >&2
  exit 2
fi

# ---- the run folder ----
mkdir -p "$OUT" || die "cannot create $OUT"
OUT=$(readlink -f "$OUT")
RUN=$OUT
if [ -e "$RUN/boot.jsonl" ] || [ -e "$RUN/stderr.log" ]; then
  die "$RUN already holds a run; name a new --out"
fi

# ---- the runtime: a build folder as is, or a checkout built here ----
if [ -f "$SRC/gta6rt.exe" ]; then
  BUILD=$SRC
elif [ -f "$SRC/runtime/CMakeLists.txt" ]; then
  [ -n "${XWIN_DIR:-}" ] || die "building $SRC needs XWIN_DIR (an xwin splat, docs/linux.md); or pass a build folder"
  BUILD=$(readlink -f -m "${BUILD_DIR:-$SRC/runtime/build/wine-game-run}")
  mkdir -p "$BUILD"
  echo "building the runtime into $BUILD"
  if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    cmake -S "$SRC/runtime" -B "$BUILD" --toolchain "$SRC/runtime/cmake/clang-cl-xwin.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo > "$RUN/configure.log" 2>&1 \
      || die "configure failed (see $RUN/configure.log)"
  fi
  cmake --build "$BUILD" --parallel --target gta6rt gta6rt_tls_host > "$RUN/build.log" 2>&1 \
    || die "build failed (see $RUN/build.log)"
else
  die "$SRC is neither a build folder with gta6rt.exe nor a checkout with runtime/CMakeLists.txt"
fi
[ -f "$BUILD/gta6rt_tls_host.dll" ] || die "no gta6rt_tls_host.dll next to $BUILD/gta6rt.exe"

# ---- one run at a time: a shared prefix has one wineserver ----
LOCK=${LOCK:-${XDG_RUNTIME_DIR:+$XDG_RUNTIME_DIR/leonida-run-game-wine.lock}}
LOCK=${LOCK:-$RUN/run.lock}
mkdir -p "$(dirname "$LOCK")" || die "cannot create lock parent for $LOCK"
exec 9>"$LOCK" || die "cannot open the lock $LOCK"
flock -n 9 || die "another run holds $LOCK"
mkdir -p "$RUN/overlay" "$RUN/saves"

# ---- environment: private session, sound off, pinned device ----
export GAME_EXE=$EXE GAME_DIR=$GAME
unset LEONIDA_GAME_EXE LEONIDA_GAME_DIR  # launcher inputs; LEONIDA_GAME_DIR also switches the runtime's tests
export WINE
export XDG_RUNTIME_DIR=$RUN/xdg
rm -rf "$XDG_RUNTIME_DIR"; mkdir -p -m 700 "$XDG_RUNTIME_DIR"
export XDG_CONFIG_HOME=$RUN/session/config XDG_DATA_HOME=$RUN/session/data XDG_CACHE_HOME=$RUN/session/cache \
       XDG_STATE_HOME=$RUN/session/state
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS
if [ "$VISIBLE" = 1 ]; then
  if command -v xdpyinfo >/dev/null 2>&1 && ! xdpyinfo >/dev/null 2>&1; then
    die "--visible: cannot open display $DISPLAY (XAUTHORITY=${XAUTHORITY:-unset})"
  fi
else
  unset DISPLAY XAUTHORITY
fi
export QT_QPA_PLATFORM=offscreen
DRM_SYSFS=${LEONIDA_DRM_SYSFS_DIR:-/sys/class/drm}
if [ -n "$DEVICE" ]; then
  export MESA_VK_DEVICE_SELECT="$DEVICE!"
  # kwin and Xwayland's glamor on the card that renders, so the present path stays on one device.
  dev_lc=${DEVICE,,}
  for n in "$DRM_SYSFS"/renderD*; do
    [ -e "$n" ] || continue
    ven=$(cat "$n/device/vendor" 2>/dev/null || true)
    did=$(cat "$n/device/device" 2>/dev/null || true)
    ven=${ven//$'\r'/}; did=${did//$'\r'/}
    [ "${ven,,}:${did,,}" = "0x${dev_lc%%:*}:0x${dev_lc#*:}" ] \
      && export KWIN_DRM_DEVICES=/dev/dri/${n##*/}
  done
else
  unset MESA_VK_DEVICE_SELECT
  echo "warning: no --device: Mesa picks the Vulkan device (pass --device VID:DID to pin it)" >&2
fi
export WINEPREFIX=${PREFIX:-$RUN/prefix} WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLOVERRIDES="winedbg.exe=d;winemenubuilder.exe=d;winepulse.drv=d;winealsa.drv=d;wineoss.drv=d;winecoreaudio.drv=d"
export PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent ALSA_CONFIG_PATH=/dev/null SDL_AUDIODRIVER=dummy
export LEONIDA_AUDIO=silent
export LEONIDA_D3D12=${LEONIDA_D3D12:-vulkan}
export LEONIDA_RUMBLE=${LEONIDA_RUMBLE:-0}
export LEONIDA_SAVE_DIR=${LEONIDA_SAVE_DIR:-Z:$RUN/saves}
if [ "$SHADERS" = none ]; then
  unset LEONIDA_SHADERS
else
  export LEONIDA_SHADERS=Z:$SHADERS
fi
env | grep -E '^(LEONIDA_|MESA_VK|WINEDLL|KWIN_DRM)' | sort > "$RUN/env.txt"

wine_bin=$(command -v "$WINE" 2>/dev/null || true)
WINESERVER=${wine_bin:+$(dirname "$wine_bin")/wineserver}
[ -n "$WINESERVER" ] && [ -x "$WINESERVER" ] || WINESERVER=wineserver
SESSION_PID=
teardown() {
  "$WINESERVER" -k 2>/dev/null
  if [ -n "$SESSION_PID" ] && kill -0 "$SESSION_PID" 2>/dev/null; then
    pkill -TERM -s "$SESSION_PID" 2>/dev/null; sleep 2; pkill -KILL -s "$SESSION_PID" 2>/dev/null
  fi
  rm -rf "$XDG_RUNTIME_DIR"
}
trap teardown EXIT
trap 'exit 130' INT TERM HUP

# The session command with its arguments baked in (kwin takes one program path).
{
  echo '#!/usr/bin/env bash'
  printf 'exec %q --inner %q %q %q %q' "$SELF" "$RUN" "$BUILD" "$LIMIT" "$SHOT_EVERY"
  for a in "$@"; do printf ' %q' "$a"; done
  echo
} > "$RUN/inner.sh"
chmod +x "$RUN/inner.sh"

T0=$(date +%s)
# setsid: Wine (and kwin, Xwayland and the private D-Bus when headless) form one session that teardown ends as a
# whole, and never the user's desktop.
if [ "$VISIBLE" = 1 ]; then
  setsid "$RUN/inner.sh" > "$RUN/session.log" 2>&1 9>&- &
else
  setsid dbus-run-session -- kwin_wayland --virtual --xwayland --socket leonida-run-game-wine --width 1920 \
      --height 1080 --no-lockscreen --no-global-shortcuts --exit-with-session "$RUN/inner.sh" \
      > "$RUN/session.log" 2>&1 9>&- &
fi
SESSION_PID=$!
# The session ends when the inner run returns; this outer limit adds a margin for the start-up.
( trap 'kill "${watch_sleep_pid:-}" 2>/dev/null || true; exit 0' TERM INT
  watch_sleep_pid=""
  sleep $((LIMIT + 180)) &
  watch_sleep_pid=$!
  wait "$watch_sleep_pid" 2>/dev/null || exit 0
  kill -TERM "$SESSION_PID" 2>/dev/null ) 9>&- </dev/null >/dev/null 2>&1 &
WATCH=$!
wait "$SESSION_PID"
pkill -P "$WATCH" 2>/dev/null || true
kill "$WATCH" 2>/dev/null || true
wait "$WATCH" 2>/dev/null || true
teardown; SESSION_PID=
T1=$(date +%s)

echo "== $(tr '\n' ';' < "$RUN/session.txt" 2>/dev/null | sed 's/;$//')"
if [ ! -f "$RUN/exit_code" ]; then
  echo "the run did not start or the session ended early ($((T1 - T0)) s); see $RUN/session.log" >&2
  exit 3
fi
python3 -I "$ROOT/tools/game_run_summary.py" "$RUN" --limit-seconds "$LIMIT" | tee "$RUN/summary.txt"
summary_rc=${PIPESTATUS[0]}
echo "output: $RUN (stderr.log, boot.jsonl, summary.txt); total $((T1 - T0)) s. The logs stay on this machine."
exit "$summary_rc"

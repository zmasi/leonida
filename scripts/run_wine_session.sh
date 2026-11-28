#!/usr/bin/env bash
# Run CI tests on the display that initialized their dedicated Wine prefix.
#
# The prefix is initialized and stopped with the Wine the tests use: $WINE (default: wine), as
# run_wine_tests.py does, so Proton's or UMU-Proton's files/bin/wine also works on a host without a
# system Wine. The wineserver is $WINESERVER, else the one next to $WINE, else wineserver on PATH.
set -e
: "${WINEPREFIX:?Set WINEPREFIX to a dedicated test prefix}"

wine=${WINE:-wine}
if ! wine_path=$(command -v -- "$wine"); then
  echo "run_wine_session.sh: Wine not found: $wine (set WINE to the wine executable)" >&2
  exit 127
fi
if [[ -z ${WINESERVER:-} ]]; then
  WINESERVER=$(dirname -- "$wine_path")/wineserver
  [[ -x $WINESERVER ]] || WINESERVER=wineserver
fi

# Stop this prefix's Wine processes before Xvfb tears down the display.
trap '"$WINESERVER" -k || true' EXIT
timeout 5m "$wine_path" wineboot --init </dev/null
"$@" </dev/null

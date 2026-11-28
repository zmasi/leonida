#!/bin/sh
# Linux and macOS counterpart of Build.bat: finds Python 3.12 or newer and runs make_game.py, which finds your copy of
# the game, checks what's installed and builds into ~/Documents/Leonida. Extra options pass through, e.g.
#   ./Build.sh --out /mnt/games/Leonida --work /mnt/scratch/leonida-work
# Nothing is installed or downloaded here; make_game.py lists anything missing (README.md, "How to build").
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd) || exit 1
. "$here/scripts/launcher_python.sh"
exec_leonida_python "$here/make_game.py" "$@"
echo "Python 3.12 or newer is needed. Install it with your package manager (Debian and Ubuntu:" >&2
echo "sudo apt install python3; macOS: brew install python), then run ./Build.sh again." >&2
exit 1

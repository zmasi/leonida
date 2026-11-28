#!/bin/sh
# Linux and macOS counterpart of Mods.bat: opens the mod manager window (tools/leonida_mods.py gui). It manages
# <port>/mods and never touches the game package. The window needs Tkinter (Debian and Ubuntu: python3-tk).
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd) || exit 1
. "$here/scripts/launcher_python.sh"
exec_leonida_python "$here/tools/leonida_mods.py" gui "$@"
echo "Python 3.12 or newer is needed. Install it with your package manager (Debian and Ubuntu:" >&2
echo "sudo apt install python3 python3-tk; macOS: brew install python), then run ./Mods.sh again." >&2
exit 1

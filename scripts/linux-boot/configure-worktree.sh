#!/bin/bash
# usage: configure-worktree.sh <worktree>
# Configures a Windows build of the runtime on Linux with clang-cl and an xwin SDK (XWIN_DIR, default
# ~/.local/share/xwin-sdk). SDL3_SRC=<an SDL3 source tree> makes the configure work offline.
set -eu

usage() {
  echo "usage: configure-worktree.sh <worktree>"
}

die() {
  echo "ERROR: $*" >&2
  exit 2
}

if [ "$#" -ge 1 ] && { [ "$1" = "-h" ] || [ "$1" = "--help" ]; }; then
  usage
  exit 0
fi

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
[ -n "$1" ] || die "worktree path must not be empty"
[ -d "$1" ] || die "worktree directory does not exist: $1"

W=$(realpath "$1")
[ -f "$W/runtime/CMakeLists.txt" ] || die "not a runtime worktree (missing runtime/CMakeLists.txt): $W"
[ -f "$W/runtime/cmake/clang-cl-xwin.cmake" ] || die "missing toolchain file: $W/runtime/cmake/clang-cl-xwin.cmake"

CMAKE=${CMAKE:-cmake}
command -v "$CMAKE" >/dev/null 2>&1 || die "cmake executable not found: $CMAKE"

if [ -n "${SDL3_SRC:-}" ] && [ ! -d "$SDL3_SRC" ]; then
  die "SDL3_SRC directory does not exist: $SDL3_SRC"
fi

cmake_args=(
  -S "$W/runtime"
  -B "$W/runtime/build/boot"
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE="$W/runtime/cmake/clang-cl-xwin.cmake"
  -DXWIN_DIR="${XWIN_DIR:-$HOME/.local/share/xwin-sdk}"
  -DLEONIDA_BUILD_WINDOWS_RUNTIME=ON
  -DLEONIDA_SDL3=ON
)
if [ -n "${SDL3_SRC:-}" ]; then
  cmake_args+=(-DFETCHCONTENT_SOURCE_DIR_SDL3="$SDL3_SRC")
fi

"$CMAKE" "${cmake_args[@]}"
